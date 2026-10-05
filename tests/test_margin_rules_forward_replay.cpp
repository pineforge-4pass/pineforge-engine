/*
 * test_margin_rules_forward_replay.cpp -- one engine, two modes: the margin,
 * sizing and admission rules decide the same trades in a backtest and in a
 * forward (stream) run, on TradingView's own tapes.
 *
 * Every rule behind pineforge::source::detail::MarginRuleSwitches lives in the
 * Pine adapter, which a batch run() and a stream (stream_begin(), then one
 * stream_push_bar() per confirmed bar) both drive. This test turns every
 * switch on and replays a sample of tapes (kSamples: sizing, placement and
 * fill admission, a long's residual / COOF / close-point calls, a short's
 * margin call, a stop entry from margin_call_rules with
 * test_margin_call_rules_tapes' RuleHost; a rejected reversal, the fill
 * re-check and a member boundary from margin_ledger_rules with
 * test_margin_ledger_rules_tapes' LedgerHost), three ways:
 *
 *   batch    run() over the chart's whole window;
 *   forward  stream_begin() over the first bar only -- the least warmup a
 *            stream accepts -- then stream_push_bar() for every later bar,
 *            then stream_end();
 *   splits   the same stream with its warmup ending after bar k for every
 *            1 < k < n, so the batch -> forward hand-over lands on every
 *            placement, fill, call and close bar of the tape.
 *
 * What is compared:
 *   - closed trades (closed_trade(i)): every field of every row -- doubles bit
 *     for bit (memcmp), integers, flags, ids and comments exactly -- forward
 *     and every split against batch;
 *   - the position still open after the last bar: its signed size, average
 *     price and the realized equity, and every open entry's direction, id,
 *     comment, time, bar index, price, size and commission, bit for bit
 *     (forward read after the last push, before stream_end());
 *   - TradingView: batch's report rows (closed trades, then the host's
 *     range-end rows) against every tape row, as test_margin_call_rules_tapes
 *     does; forward's closed trades against the tape's closed rows, and its
 *     open entries against the entry half of the tape's range-end rows (the
 *     Exit rows with an empty Signal): side, lots, entry ticks, entry time.
 *
 * Range-end rows are not compared between the modes. They are a report-only
 * mark to market of a position still open after the last bar: a batch run
 * writes them at its terminal bar, while a forward run has no last bar
 * until stream_end() declares the end of data, and they never enter
 * strategy.closedtrades. The open position they are cut from is compared
 * instead, bit for bit; the forward run's own range-end rows are printed.
 *
 * The identity is required with every switch on, for every streamed sample
 * (kBatchDiffersFromTv lists a sampled tape whose batch run does not match
 * TradingView at this tree; its batch == forward identity is still required).
 * As a control, every switch off: the identity again, and a sample's forward
 * trades must move between switches off and on exactly when its batch trades
 * do, so the rules are live in the stream, not merely absent from both modes
 * (kRulesOffModeDivergences lists a tape whose modes differ with the rules off).
 * And the switch set the tree ships (MarginRuleSwitches' defaults, every rule
 * on): the identity on the sample and on every hand-over point, and
 * batch == TradingView unless kShippedDiffersFromTv lists the tape.
 *
 * kStreamRefusals lists a configuration a stream refuses, with the refusal
 * text, which then stays out of the forward half: a stream recalculates on
 * bar close only, so stream_begin() refuses calc_on_order_fills. A listed
 * entry that stops applying fails the test until it is removed.
 *
 * kSamples is the extension point: a row names the fixture (margin_call_rules
 * or margin_ledger_rules, each with its own cases.tsv columns and host), a
 * tape path in it and what the tape covers.
 */

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#ifndef PINEFORGE_TEST_FIXTURES_DIR
#error "PINEFORGE_TEST_FIXTURES_DIR must name tests/fixtures"
#endif

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kMinute = 60'000;
// cases.tsv's qty column: "percent" (default_qty_type percent of equity, 100)
// and "source_raw" (the script's floor(equity / close / step) * step).
constexpr double kPercentQty = -1.0;
constexpr double kSourceRawQty = -2.0;

struct Sample {
    const char* fixture;  // a directory under tests/fixtures
    const char* path;     // the tape's path in that fixture's cases.tsv
    const char* covers;
};

const std::vector<Sample> kSamples = {
    {"margin_call_rules", "sizing/size-btc-p3u", "sizing"},
    {"margin_call_rules", "boundary/size-p1u", "sizing"},
    {"margin_call_rules", "admit/admit-eur-short-ulp-p2u", "placement admission"},
    {"margin_call_rules", "boundary/long-admit-m1u", "placement admission"},
    {"margin_call_rules", "admit/admit-eth-nonpooc-s2-m04u", "fill admission"},
    {"margin_call_rules", "admit/admit-eth-nonpooc-s2-gapup", "fill admission"},
    {"margin_call_rules", "literals/literal-24", "long residual call"},
    {"margin_call_rules", "coof/coof-low-call-on", "COOF next-point close"},
    {"margin_call_rules", "sizing/size-eth-06-22-t1", "close-point sizing"},
    {"margin_call_rules", "quantum/quantum-s5-m4", "short margin call"},
    {"margin_call_rules", "stopband/stopband-stop-p10u", "stop entry"},
    {"margin_call_rules", "short-cutoff-gate/cutoff-q78381-s1-d0280",
     "short call at the high, lagged follow-up at the close"},
    {"margin_call_rules", "short-cutoff-gate/gateedge-q78381-f5-d027200", "short call gate"},
    {"margin_call_rules", "short-cutoff-gate/holdout-q12345-s5-f2-d023000",
     "short call gate, slippage 5"},
    {"margin_call_rules", "literals/literal-21", "close call's follow-up at the open"},
    // Event-ledger tapes (LedgerHost, test_margin_ledger_rules_tapes' probe).
    {"margin_ledger_rules", "c1-r1-exp-l2s-reject/r1", "rejected reversal"},
    {"margin_ledger_rules", "c1-b1-exp-long-below-Tm1ulp/r1", "fill re-check"},
    {"margin_ledger_rules", "c1-b2-exp-long-above-T/r1", "fill re-check"},
    {"margin_ledger_rules", "f-antoni-history-k0-only/r1", "member boundary"},
};

constexpr const char* kLedgerFixture = "margin_ledger_rules";

// Sampled tapes whose batch run differs from TradingView at this tree:
// "fixture/path" and a one-line reason.
const std::vector<std::pair<std::string, std::string>> kBatchDiffersFromTv = {
};

// Sampled tapes whose batch run differs from TradingView with the shipped
// switch set (every rule on): "fixture/path" and a one-line reason.
const std::vector<std::pair<std::string, std::string>> kShippedDiffersFromTv = {
};

// Sampled tapes whose replays differ between the modes with every switch OFF
// (the control, not the binding claim): "fixture/path" and what differs.
const std::vector<std::pair<std::string, std::string>> kRulesOffModeDivergences = {
};

// Configurations a stream refuses: "fixture/path", a fragment of the refusal
// text, and the reason.
struct Refusal {
    const char* key;
    const char* error_fragment;
    const char* reason;
};
const std::vector<Refusal> kStreamRefusals = {
    {"margin_call_rules/boundary/long-admit-m1u", "calc_on_order_fills is unsupported",
     "calc_on_order_fills: a stream recalculates on bar close only"},
    {"margin_call_rules/literals/literal-24", "calc_on_order_fills is unsupported",
     "calc_on_order_fills: a stream recalculates on bar close only"},
    {"margin_call_rules/coof/coof-low-call-on", "calc_on_order_fills is unsupported",
     "calc_on_order_fills: a stream recalculates on bar close only"},
};

std::string fixture(const std::string& name, const std::string& relative) {
    return std::string(PINEFORGE_TEST_FIXTURES_DIR) + "/" + name + "/" + relative;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> cells;
    std::string cell;
    std::istringstream fields(text);
    while (std::getline(fields, cell, separator)) cells.push_back(cell);
    if (!text.empty() && text.back() == separator) cells.emplace_back();
    return cells;
}

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// A tape stamp "YYYY-MM-DD HH:MM" rendered at UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * kMinute;
}

struct LedgerTrade {
    std::int64_t entry_time;
    std::int64_t close_time;
    bool is_long;
    double quantity;
};

struct Case {
    std::string fixture;
    std::string path;
    std::string csv;  // the tape, relative to the fixture
    std::string timeframe = "15";
    std::string bars;
    double tick = 0.0;
    double step = 0.0;
    double capital = 0.0;
    bool is_long = true;
    double qty = 0.0;
    int slippage = 0;
    double fee = 0.0;
    bool cash_per_contract = false;
    double margin = 100.0;
    bool pooc = false;
    bool coof = false;
    std::int64_t entry_ms = 0;
    std::int64_t close_ms = 0;
    std::int64_t to_ms = 0;
    double stop = kNaN;
    std::vector<LedgerTrade> prior;
    std::vector<double> lots;
    int pyramiding = 1;
};

// One fixture's cases.tsv (test_margin_call_rules_tapes' parser).
std::vector<Case> load_cases(const std::string& name) {
    std::ifstream in(fixture(name, "cases.tsv"));
    CHECK(in.good());
    std::string line;
    std::getline(in, line);
    std::map<std::string, std::size_t> column;
    const std::vector<std::string> header = split(line, '\t');
    for (std::size_t i = 0; i < header.size(); ++i) column[header[i]] = i;
    for (const char* key : {"path", "bars", "tick", "step", "capital", "direction", "qty",
                            "slippage", "fee", "fee_type", "margin", "pooc", "coof", "entry_ms",
                            "close_ms", "to_ms", "stop", "prior", "lots", "pyramiding"}) {
        CHECK(column.count(key) == 1);
        if (!column.count(key)) return {};
    }
    std::vector<Case> cases;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> cell = split(line, '\t');
        CHECK(cell.size() == header.size());
        if (cell.size() != header.size()) continue;
        auto at = [&](const char* key) -> const std::string& { return cell[column[key]]; };
        auto number = [&](const char* key) { return std::strtod(at(key).c_str(), nullptr); };
        auto ms = [&](const std::string& text) {
            return static_cast<std::int64_t>(std::strtoll(text.c_str(), nullptr, 10));
        };
        Case c;
        c.fixture = name;
        c.path = at("path");
        c.csv = c.path + "/tv_trades.csv";
        c.bars = at("bars");
        c.tick = number("tick");
        c.step = number("step");
        c.capital = number("capital");
        c.is_long = at("direction") == "long";
        CHECK(at("direction") == "long" || at("direction") == "short");
        c.qty = at("qty") == "percent" ? kPercentQty
              : at("qty") == "source_raw" ? kSourceRawQty : number("qty");
        c.slippage = std::atoi(at("slippage").c_str());
        c.fee = number("fee");
        c.cash_per_contract = at("fee_type") == "cash_per_contract";
        CHECK(at("fee_type") == "percent" || at("fee_type") == "cash_per_contract");
        c.margin = number("margin");
        c.pooc = at("pooc") == "1";
        c.coof = at("coof") == "1";
        c.entry_ms = ms(at("entry_ms"));
        c.close_ms = ms(at("close_ms"));
        c.to_ms = ms(at("to_ms"));
        if (!at("stop").empty()) c.stop = number("stop");
        if (!at("prior").empty()) {
            for (const std::string& trade : split(at("prior"), ';')) {
                const std::vector<std::string> part = split(trade, ':');
                CHECK(part.size() == 4);
                if (part.size() != 4) continue;
                c.prior.push_back({ms(part[0]), ms(part[1]), part[2] == "long",
                                   std::strtod(part[3].c_str(), nullptr)});
            }
        }
        if (!at("lots").empty()) {
            for (const std::string& lot : split(at("lots"), ';'))
                c.lots.push_back(std::strtod(lot.c_str(), nullptr));
        }
        c.pyramiding = std::atoi(at("pyramiding").c_str());
        cases.push_back(std::move(c));
    }
    return cases;
}

// Every row of a bars/ file: the chart's window, from 00:00 UTC through to
// 00:00 UTC inclusive.
std::vector<Bar> load_bars(const std::string& name, const std::string& file) {
    std::ifstream in(fixture(name, "bars/" + file));
    CHECK(in.good());
    std::string line;
    std::getline(in, line);
    CHECK(line == "timestamp,open,high,low,close,volume");
    std::vector<Bar> bars;
    while (std::getline(in, line)) {
        const std::vector<std::string> cell = split(line, ',');
        CHECK(cell.size() == 6);
        if (cell.size() != 6) continue;
        bars.push_back({std::strtod(cell[1].c_str(), nullptr), std::strtod(cell[2].c_str(), nullptr),
                        std::strtod(cell[3].c_str(), nullptr), std::strtod(cell[4].c_str(), nullptr),
                        std::strtod(cell[5].c_str(), nullptr),
                        static_cast<std::int64_t>(std::strtoll(cell[0].c_str(), nullptr, 10))});
    }
    return bars;
}

// One trade row as both sides report it: prices in ticks, quantity in lots.
struct Row {
    bool is_long = true;
    long long qty_lots = 0;
    long long entry_ticks = 0;
    long long exit_ticks = 0;
    std::int64_t entry_ms = 0;
    std::int64_t exit_ms = 0;
    bool range_end = false;  // TradingView: the Exit row's Signal is empty
};

template <typename C>
Row make_row(bool is_long, double qty, double entry_price, double exit_price,
             std::int64_t entry_ms, std::int64_t exit_ms, const C& c) {
    Row row;
    row.is_long = is_long;
    row.qty_lots = std::llround(std::fabs(qty) / c.step);
    row.entry_ticks = std::llround(entry_price / c.tick);
    row.exit_ticks = std::llround(exit_price / c.tick);
    row.entry_ms = entry_ms;
    row.exit_ms = exit_ms;
    return row;
}

// TradingView's rows: each Exit row paired with the Entry row of its trade
// number, in trade-number order. tv_trades.csv starts with a UTF-8 BOM; its
// header is "Trade number,Type,Date and time,Signal,Price <CCY>,Size (qty),...".
template <typename C>
std::vector<Row> tape_rows(const C& c) {
    std::ifstream in(fixture(c.fixture, c.csv));
    CHECK(in.good());
    std::string line;
    std::getline(in, line);
    if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
    CHECK(line.rfind("Trade number,Type,Date and time,Signal,Price ", 0) == 0);
    struct Side {
        bool seen = false;
        bool is_long = true;
        double price = 0.0;
        double qty = 0.0;
        std::int64_t ms = 0;
        bool empty_signal = false;
    };
    std::map<int, std::pair<Side, Side>> by_number;  // (entry, exit)
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::vector<std::string> cell = split(line, ',');
        CHECK(cell.size() >= 6);
        if (cell.size() < 6) continue;
        const int number = std::atoi(cell[0].c_str());
        const bool entry = cell[1].rfind("Entry", 0) == 0;
        CHECK(entry || cell[1].rfind("Exit", 0) == 0);
        Side& side = entry ? by_number[number].first : by_number[number].second;
        CHECK(!side.seen);
        side.seen = true;
        side.is_long = cell[1].size() >= 4 && cell[1].compare(cell[1].size() - 4, 4, "long") == 0;
        side.ms = tape_ms(cell[2]);
        side.empty_signal = cell[3].empty();
        side.price = std::strtod(cell[4].c_str(), nullptr);
        side.qty = std::strtod(cell[5].c_str(), nullptr);
    }
    std::vector<Row> rows;
    for (const auto& [number, pair] : by_number) {
        const Side& entry = pair.first;
        const Side& exit = pair.second;
        CHECK(entry.seen && exit.seen);
        if (!entry.seen || !exit.seen) continue;
        CHECK(entry.is_long == exit.is_long);
        Row row = make_row(exit.is_long, exit.qty, entry.price, exit.price, entry.ms, exit.ms, c);
        row.range_end = exit.empty_signal;
        rows.push_back(row);
    }
    return rows;
}

// One open entry of the position still open after the last bar.
struct OpenEntry {
    std::string direction;
    std::string id;
    std::string comment;
    std::int64_t time = 0;
    int bar_index = -1;
    double price = 0.0;
    double size = 0.0;
    double commission = 0.0;
};

// What a replay leaves: its closed trades, its report's range-end rows and
// the position still open.
struct Snapshot {
    std::string error;
    std::vector<Trade> closed;
    std::vector<Trade> range_end;
    double position = 0.0;
    double avg_price = 0.0;
    double equity = 0.0;
    std::vector<OpenEntry> open;
};

// A Pine strategy host that reads back what a replay leaves.
class ReplayHost : public source::PineStrategyHost {
public:
    // Closed trades (strategy.closedtrades) and the report's range-end rows.
    void read_trades(Snapshot& s) const {
        s.closed.clear();
        s.range_end.clear();
        for (std::size_t i = 0; i < closed_trade_count(); ++i) s.closed.push_back(closed_trade(i));
        for (int i = static_cast<int>(closed_trade_count()); i < report_trade_count(); ++i)
            s.range_end.push_back(get_report_trade(i));
    }

    // The position still open (strategy.position_size, position_avg_price,
    // strategy.opentrades.*) and the realized equity.
    void read_position(Snapshot& s) const {
        s.position = live_position_size();
        s.avg_price = position_avg_price();
        s.equity = live_current_equity();
        s.open.clear();
        for (int i = 0; !open_trade_direction(i).empty(); ++i) {
            OpenEntry entry;
            entry.direction = open_trade_direction(i);
            entry.id = open_trade_entry_id(i);
            entry.comment = open_trade_entry_comment(i);
            entry.time = open_trade_entry_time(i);
            entry.bar_index = open_trade_entry_bar_index(i);
            entry.price = open_trade_entry_price(i);
            entry.size = open_trade_size(i);
            entry.commission = open_trade_commission(i);
            s.open.push_back(std::move(entry));
        }
    }

};

// engine_harness.cpp's probe host (counter_harness.cpp's when `lots` is set),
// configured from one cases.tsv row: test_margin_call_rules_tapes' RuleHost.
class RuleHost final : public ReplayHost {
public:
    explicit RuleHost(const Case& c) : c_(c) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig configuration;
        configuration.initial_capital = c.capital;
        configuration.pyramiding = c.pyramiding;
        configuration.margin_long = c.margin;
        configuration.margin_short = c.margin;
        configuration.commission_type = static_cast<int>(
            c.cash_per_contract ? CommissionType::CASH_PER_CONTRACT : CommissionType::PERCENT);
        configuration.commission_value = c.fee;
        configuration.slippage = c.slippage;
        configuration.process_orders_on_close = c.pooc;
        configuration.calc_on_order_fills = c.coof;
        if (c.qty == kPercentQty) {
            configuration.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
            configuration.default_qty_value = 100.0;
        }
        configure_pine_strategy(configuration);
        set_syminfo_mintick(c.tick);
        set_syminfo_metadata("qty_step", c.step);
    }

    void on_source_bar(const Bar& bar) override {
        if (!c_.lots.empty()) {
            // counter_harness.cpp: one same-bar batch "A", "B", "C", ... then close_all.
            if (bar.timestamp == c_.entry_ms) {
                for (std::size_t lot = 0; lot < c_.lots.size(); ++lot)
                    strategy_entry(std::string(1, static_cast<char>('A' + lot)), c_.is_long,
                                   kNaN, kNaN, c_.lots[lot]);
            }
            if (bar.timestamp == c_.close_ms) strategy_close_all();
            return;
        }
        for (std::size_t index = 0; index < c_.prior.size(); ++index) {
            const LedgerTrade& trade = c_.prior[index];
            if (bar.timestamp == trade.entry_time && signed_position_size() == 0.0)
                strategy_entry("ledger-" + std::to_string(index), trade.is_long, kNaN, kNaN,
                               trade.quantity);
            if (bar.timestamp == trade.close_time) strategy_close_all();
        }
        if (bar.timestamp == c_.entry_ms && signed_position_size() == 0.0) {
            const double quantity =
                c_.qty == kSourceRawQty ? std::floor(current_equity() / bar.close / c_.step) * c_.step
                : c_.qty < 0.0          ? kNaN
                                        : c_.qty;
            strategy_entry("probe", c_.is_long, kNaN, c_.stop, quantity);
        }
        if (bar.timestamp == c_.close_ms) strategy_close_all();
    }

private:
    Case c_;
};

// One row of margin_ledger_rules/cases.tsv (test_margin_ledger_rules_tapes'
// parser): the probe's scripted calls at bar opens.
enum class Op { Entry, Close, CloseAll };

struct Event {
    std::int64_t time = 0;
    Op op = Op::Entry;
    std::string id;
    bool is_long = true;
    double qty = kNaN;
    bool boundary = false;
};

struct LedgerCase {
    std::string fixture;
    std::string path;  // the tape's directory, <name>/<rK>
    std::string csv;   // the tape, relative to the fixture
    std::string timeframe;
    std::string bars;
    std::string symbol;
    double tick = 0.0;
    double step = 0.0;
    double capital = 0.0;
    double margin_long = 100.0;
    double margin_short = 100.0;
    bool after_close = false;
    std::vector<Event> events;
};

std::vector<LedgerCase> load_ledger_cases(const std::string& name) {
    std::ifstream in(fixture(name, "cases.tsv"));
    CHECK(in.good());
    std::string line;
    std::getline(in, line);
    std::map<std::string, std::size_t> column;
    const std::vector<std::string> header = split(line, '\t');
    for (std::size_t i = 0; i < header.size(); ++i) column[header[i]] = i;
    for (const char* key : {"name", "tape", "bars", "symbol", "timeframe", "tick", "step",
                            "capital", "margin_long", "margin_short", "after_close", "events"}) {
        CHECK(column.count(key) == 1);
        if (!column.count(key)) return {};
    }
    std::vector<LedgerCase> cases;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> cell = split(line, '\t');
        CHECK(cell.size() == header.size());
        if (cell.size() != header.size()) continue;
        auto at = [&](const char* key) -> const std::string& { return cell[column[key]]; };
        auto number = [&](const char* key) { return std::strtod(at(key).c_str(), nullptr); };
        LedgerCase c;
        c.fixture = name;
        c.csv = at("tape");
        const std::string suffix = "/tv_trades.csv";
        CHECK(c.csv.size() > suffix.size() &&
              c.csv.compare(c.csv.size() - suffix.size(), suffix.size(), suffix) == 0);
        if (c.csv.size() <= suffix.size()) continue;
        c.path = c.csv.substr(0, c.csv.size() - suffix.size());
        c.bars = at("bars");
        c.symbol = at("symbol");
        c.timeframe = at("timeframe");
        CHECK(c.timeframe == "15" || c.timeframe == "1D");
        c.tick = number("tick");
        c.step = number("step");
        c.capital = number("capital");
        c.margin_long = number("margin_long");
        c.margin_short = number("margin_short");
        c.after_close = at("after_close") == "1";
        for (const std::string& text : split(at("events"), ';')) {
            // time:op:id:side:qty:boundary
            const std::vector<std::string> part = split(text, ':');
            CHECK(part.size() == 6);
            if (part.size() != 6) continue;
            Event event;
            event.time = static_cast<std::int64_t>(std::strtoll(part[0].c_str(), nullptr, 10));
            CHECK(part[1] == "entry" || part[1] == "close" || part[1] == "close_all");
            event.op = part[1] == "entry" ? Op::Entry : part[1] == "close" ? Op::Close : Op::CloseAll;
            event.id = part[2];
            event.is_long = part[3] == "1";
            if (!part[4].empty()) event.qty = std::strtod(part[4].c_str(), nullptr);
            event.boundary = part[5] == "1";
            c.events.push_back(std::move(event));
        }
        CHECK(!c.events.empty());
        cases.push_back(std::move(c));
    }
    return cases;
}

// test_margin_ledger_rules_tapes' LedgerHost: strategy() with the row's
// capital and margins, 100 % of equity by default, no commission, no
// slippage; the symbol's mintick and quantity step, and for the stocks the
// exchange's time zone and regular session.
class LedgerHost final : public ReplayHost {
public:
    explicit LedgerHost(const LedgerCase& c) : c_(c) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig configuration;
        configuration.initial_capital = c.capital;
        configuration.margin_long = c.margin_long;
        configuration.margin_short = c.margin_short;
        configuration.commission_type = static_cast<int>(CommissionType::PERCENT);
        configuration.commission_value = 0.0;
        configuration.slippage = 0;
        configuration.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        configuration.default_qty_value = 100.0;
        configure_pine_strategy(configuration);
        set_syminfo_mintick(c.tick);
        set_syminfo_metadata("qty_step", c.step);
        if (c.symbol.rfind("NYSE:", 0) == 0 || c.symbol.rfind("NASDAQ:", 0) == 0) {
            set_syminfo_timezone("America/New_York");
            set_syminfo_session("0930-1600");
        }
        for (std::size_t index = 0; index < c_.events.size(); ++index)
            at_time_[c_.events[index].time].push_back(index);
    }

    // The calls whose time is this bar's open, in the script's statement
    // order; an after_close probe closes "Seed" right after its boundary entry.
    void on_source_bar(const Bar& bar) override {
        const auto due = at_time_.find(bar.timestamp);
        if (due == at_time_.end()) return;
        for (const std::size_t index : due->second) {
            const Event& event = c_.events[index];
            switch (event.op) {
            case Op::Entry:
                strategy_entry(event.id, event.is_long, kNaN, kNaN, event.qty);
                break;
            case Op::Close:
                strategy_close(event.id);
                break;
            case Op::CloseAll:
                strategy_close_all();
                break;
            }
            if (event.boundary && c_.after_close) strategy_close("Seed");
        }
    }

private:
    LedgerCase c_;
    std::map<std::int64_t, std::vector<std::size_t>> at_time_;
};

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

std::string number_text(double value) {
    char text[64];
    std::snprintf(text, sizeof text, "%.17g", value);
    return text;
}

// The first field two closed-trade rows differ in, or "" when every field of
// Trade is identical (doubles bit for bit).
std::string trade_difference(const Trade& a, const Trade& b) {
    auto real = [](const char* name, double x, double y) -> std::string {
        return same_bits(x, y) ? std::string()
                               : std::string(name) + " " + number_text(x) + " vs " + number_text(y);
    };
    auto whole = [](const char* name, long long x, long long y) -> std::string {
        return x == y ? std::string()
                      : std::string(name) + " " + std::to_string(x) + " vs " + std::to_string(y);
    };
    auto text = [](const char* name, const std::string& x, const std::string& y) -> std::string {
        return x == y ? std::string() : std::string(name) + " '" + x + "' vs '" + y + "'";
    };
    const std::string differences[] = {
        whole("entry_time", a.entry_time, b.entry_time),
        whole("exit_time", a.exit_time, b.exit_time),
        real("entry_price", a.entry_price, b.entry_price),
        real("exit_price", a.exit_price, b.exit_price),
        real("qty", a.qty, b.qty),
        real("pnl", a.pnl, b.pnl),
        real("pnl_pct", a.pnl_pct, b.pnl_pct),
        whole("is_long", a.is_long, b.is_long),
        whole("entry_bar_index", a.entry_bar_index, b.entry_bar_index),
        whole("exit_bar_index", a.exit_bar_index, b.exit_bar_index),
        text("entry_id", a.entry_id, b.entry_id),
        text("entry_comment", a.entry_comment, b.entry_comment),
        text("exit_comment", a.exit_comment, b.exit_comment),
        text("exit_id", a.exit_id, b.exit_id),
        whole("exit_from_bracket", a.exit_from_bracket, b.exit_from_bracket),
        real("max_runup", a.max_runup, b.max_runup),
        real("max_drawdown", a.max_drawdown, b.max_drawdown),
        real("commission", a.commission, b.commission),
        whole("entry_incarnation", static_cast<long long>(a.entry_incarnation),
              static_cast<long long>(b.entry_incarnation)),
        whole("open_at_end", a.open_at_end, b.open_at_end),
        whole("close_cause", static_cast<long long>(a.close_cause),
              static_cast<long long>(b.close_cause)),
    };
    for (const std::string& difference : differences)
        if (!difference.empty()) return difference;
    return {};
}

// The first difference between a batch and a forward replay's closed trades
// and open position, or "".
std::string replay_difference(const Snapshot& batch, const Snapshot& forward) {
    if (!batch.error.empty()) return "batch error: " + batch.error;
    if (!forward.error.empty()) return forward.error;
    if (batch.closed.size() != forward.closed.size())
        return "closed trades " + std::to_string(batch.closed.size()) + " vs " +
               std::to_string(forward.closed.size());
    for (std::size_t i = 0; i < batch.closed.size(); ++i) {
        const std::string difference = trade_difference(batch.closed[i], forward.closed[i]);
        if (!difference.empty()) return "closed trade " + std::to_string(i + 1) + " " + difference;
    }
    if (!same_bits(batch.position, forward.position))
        return "position size " + number_text(batch.position) + " vs " + number_text(forward.position);
    if (!same_bits(batch.avg_price, forward.avg_price))
        return "position avg price " + number_text(batch.avg_price) + " vs " +
               number_text(forward.avg_price);
    if (!same_bits(batch.equity, forward.equity))
        return "realized equity " + number_text(batch.equity) + " vs " + number_text(forward.equity);
    if (batch.open.size() != forward.open.size())
        return "open entries " + std::to_string(batch.open.size()) + " vs " +
               std::to_string(forward.open.size());
    for (std::size_t i = 0; i < batch.open.size(); ++i) {
        const OpenEntry& a = batch.open[i];
        const OpenEntry& b = forward.open[i];
        const std::string at = "open entry " + std::to_string(i + 1) + " ";
        if (a.direction != b.direction) return at + "direction " + a.direction + " vs " + b.direction;
        if (a.id != b.id) return at + "id '" + a.id + "' vs '" + b.id + "'";
        if (a.comment != b.comment) return at + "comment '" + a.comment + "' vs '" + b.comment + "'";
        if (a.time != b.time) return at + "time " + std::to_string(a.time) + " vs " + std::to_string(b.time);
        if (a.bar_index != b.bar_index)
            return at + "bar index " + std::to_string(a.bar_index) + " vs " + std::to_string(b.bar_index);
        if (!same_bits(a.price, b.price))
            return at + "price " + number_text(a.price) + " vs " + number_text(b.price);
        if (!same_bits(a.size, b.size))
            return at + "size " + number_text(a.size) + " vs " + number_text(b.size);
        if (!same_bits(a.commission, b.commission))
            return at + "commission " + number_text(a.commission) + " vs " + number_text(b.commission);
    }
    return {};
}

std::string row_difference(std::size_t index, const Row& tv, const Row& engine, bool with_exit) {
    auto field = [&](const char* name, long long expected, long long actual) {
        return "row " + std::to_string(index + 1) + " " + name + " tv=" + std::to_string(expected) +
               " engine=" + std::to_string(actual);
    };
    if (tv.is_long != engine.is_long) return field("long", tv.is_long, engine.is_long);
    if (tv.qty_lots != engine.qty_lots) return field("qty_lots", tv.qty_lots, engine.qty_lots);
    if (tv.entry_ticks != engine.entry_ticks)
        return field("entry_ticks", tv.entry_ticks, engine.entry_ticks);
    if (tv.entry_ms != engine.entry_ms) return field("entry_ms", tv.entry_ms, engine.entry_ms);
    if (with_exit && tv.exit_ticks != engine.exit_ticks)
        return field("exit_ticks", tv.exit_ticks, engine.exit_ticks);
    if (with_exit && tv.exit_ms != engine.exit_ms) return field("exit_ms", tv.exit_ms, engine.exit_ms);
    return {};
}

// Batch against every tape row: its report rows, closed trades then the
// host's range-end rows.
template <typename C>
std::string batch_tv_difference(const std::vector<Row>& tv, const Snapshot& batch, const C& c) {
    if (!batch.error.empty()) return "engine error: " + batch.error;
    std::vector<Row> rows;
    for (const Trade& t : batch.closed)
        rows.push_back(make_row(t.is_long, t.qty, t.entry_price, t.exit_price, t.entry_time,
                                t.exit_time, c));
    for (const Trade& t : batch.range_end)
        rows.push_back(make_row(t.is_long, t.qty, t.entry_price, t.exit_price, t.entry_time,
                                t.exit_time, c));
    if (tv.size() != rows.size())
        return "rows tv=" + std::to_string(tv.size()) + " engine=" + std::to_string(rows.size());
    for (std::size_t i = 0; i < tv.size(); ++i) {
        const std::string difference = row_difference(i, tv[i], rows[i], true);
        if (!difference.empty()) return difference;
    }
    return {};
}

// Forward against the tape: its closed trades are the tape's closed rows, its
// open entries the entry half of the tape's range-end rows.
template <typename C>
std::string forward_tv_difference(const std::vector<Row>& tv, const Snapshot& forward,
                                  const C& c) {
    if (!forward.error.empty()) return forward.error;
    std::vector<Row> closed;
    std::vector<Row> range_end;
    for (const Row& row : tv) {
        if (row.range_end) {
            range_end.push_back(row);
        } else {
            if (!range_end.empty()) return "a tape range-end row precedes a closed row";
            closed.push_back(row);
        }
    }
    if (closed.size() != forward.closed.size())
        return "closed rows tv=" + std::to_string(closed.size()) +
               " forward=" + std::to_string(forward.closed.size());
    for (std::size_t i = 0; i < closed.size(); ++i) {
        const Trade& t = forward.closed[i];
        const std::string difference = row_difference(
            i, closed[i],
            make_row(t.is_long, t.qty, t.entry_price, t.exit_price, t.entry_time, t.exit_time, c),
            true);
        if (!difference.empty()) return "closed " + difference;
    }
    if (range_end.size() != forward.open.size())
        return "range-end rows tv=" + std::to_string(range_end.size()) +
               " forward open entries=" + std::to_string(forward.open.size());
    for (std::size_t i = 0; i < range_end.size(); ++i) {
        const OpenEntry& e = forward.open[i];
        const std::string difference = row_difference(
            closed.size() + i, range_end[i],
            make_row(e.direction == "long", e.size, e.price, 0.0, e.time, 0, c), false);
        if (!difference.empty()) return "open entry vs range-end " + difference;
    }
    return {};
}

void dump(const char* label, const Snapshot& s) {
    auto row = [&](const char* kind, const Trade& t) {
        std::printf("    %s %s %s qty=%.17g entry=%lld@%.17g exit=%lld@%.17g pnl=%.17g id='%s' "
                    "exit_id='%s' exit_comment='%s' bars=%d..%d\n",
                    label, kind, t.is_long ? "long" : "short", t.qty,
                    static_cast<long long>(t.entry_time), t.entry_price,
                    static_cast<long long>(t.exit_time), t.exit_price, t.pnl, t.entry_id.c_str(),
                    t.exit_id.c_str(), t.exit_comment.c_str(), t.entry_bar_index, t.exit_bar_index);
    };
    for (const Trade& t : s.closed) row("closed", t);
    for (const Trade& t : s.range_end) row("range-end", t);
    for (const OpenEntry& e : s.open)
        std::printf("    %s open %s size=%.17g entry=%lld@%.17g id='%s' bar=%d\n", label,
                    e.direction.c_str(), e.size, static_cast<long long>(e.time), e.price,
                    e.id.c_str(), e.bar_index);
    std::printf("    %s position=%.17g equity=%.17g\n", label, s.position, s.equity);
}

template <typename Host, typename C>
Snapshot batch_replay(const C& c, const std::vector<Bar>& bars) {
    Host host(c);
    host.run(bars.data(), static_cast<int>(bars.size()), c.timeframe, c.timeframe);
    Snapshot s;
    s.error = host.last_error();
    host.read_trades(s);
    host.read_position(s);
    return s;
}

// stream_begin() over bars[0, warmup), stream_push_bar() for every later bar,
// the position read after the last push, then stream_end(), which must book
// no closed trade and leave every closed row as it was.
template <typename Host, typename C>
Snapshot forward_replay(const C& c, const std::vector<Bar>& bars, std::size_t warmup) {
    Host host(c);
    Snapshot s;
    if (!host.stream_begin(bars.data(), static_cast<int>(warmup), c.timeframe, c.timeframe)) {
        s.error = "stream_begin refused: " + host.last_error();
        return s;
    }
    if (!host.stream_is_realtime()) {
        s.error = "stream_begin did not reach realtime";
        return s;
    }
    for (std::size_t i = warmup; i < bars.size(); ++i) {
        if (!host.stream_push_bar(bars[i])) {
            s.error = "stream_push_bar(" + std::to_string(i) + ") refused: " + host.last_error();
            return s;
        }
    }
    host.read_trades(s);
    host.read_position(s);
    if (!host.stream_end()) {
        s.error = "stream_end refused: " + host.last_error();
        return s;
    }
    Snapshot ended;
    host.read_trades(ended);
    if (ended.closed.size() != s.closed.size()) {
        s.error = "stream_end changed the closed trades";
        return s;
    }
    for (std::size_t i = 0; i < s.closed.size(); ++i) {
        if (!trade_difference(s.closed[i], ended.closed[i]).empty()) {
            s.error = "stream_end changed closed trade " + std::to_string(i + 1);
            return s;
        }
    }
    s.range_end = ended.range_end;
    return s;
}

using pineforge::source::detail::MarginRuleSwitches;

// Every switch set to `value`. A new MarginRuleSwitches field must be set here
// too: the static_assert stops the build until it is.
template <typename S>
S every_rule(bool value) {
    static_assert(sizeof(S) == 17 && alignof(S) == 1,
                  "MarginRuleSwitches gained a field: set it in every_rule()");
    S s;
    s.decimal_sizing = value;
    s.slipped_signal_admission = value;
    s.unified_placement = value;
    s.fill_price_recheck = value;
    s.close_first_admission = value;
    s.gain_loss_money = value;
    s.dust_unit_call = value;
    s.long_open_close_checks = value;
    s.negative_free_cash_call = value;
    s.coof_next_point_close = value;
    s.close_point_reversal = value;
    s.pooc_fee_sizing = value;
    s.point_fills_before_margin = value;
    s.point_order_trailing_exits = value;
    s.pyramiding_ledger_records = value;
    s.exit_child_tombstones = value;
    s.tie_reversal_beside_exits = value;
    return s;
}

template <typename S>
bool same_switches(const S& a, const S& b) {
    return std::memcmp(&a, &b, sizeof(S)) == 0;
}

struct Tables {
    std::map<std::string, std::string> differs_from_tv;
    std::map<std::string, std::string> shipped_differs_from_tv;
    std::map<std::string, std::string> off_divergences;
    std::map<std::string, const Refusal*> refusals;
};

struct Totals {
    std::size_t streamed = 0;
    std::size_t splits_identical = 0;
    std::size_t splits_total = 0;
    std::size_t rules_move_trades = 0;
    std::size_t tv_matched = 0;
    std::size_t shipped_streamed = 0;
    std::size_t shipped_splits_identical = 0;
    std::size_t shipped_splits_total = 0;
    std::size_t shipped_tv_matched = 0;
};

// One sampled tape: batch, forward, every split and the rules-off control.
template <typename Host, typename C>
void replay_sample(const Sample& sample, const C& c, const std::vector<Bar>& bars,
                   const Tables& tables, Totals& totals) {
    auto& switches = pineforge::source::detail::margin_rule_switches();
    const std::string key = std::string(sample.fixture) + "/" + sample.path;
    CHECK(bars.size() >= 2);
    if (bars.size() < 2) return;
    const std::vector<Row> tv = tape_rows(c);

    // Every rule on: batch against TradingView.
    switches = every_rule<MarginRuleSwitches>(true);
    const Snapshot batch = batch_replay<Host>(c, bars);
    CHECK(batch.error.empty());
    const std::string batch_tv = batch_tv_difference(tv, batch, c);
    const bool batch_matches_tv = batch_tv.empty();
    const auto listed = tables.differs_from_tv.find(key);
    if (listed == tables.differs_from_tv.end()) {
        CHECK(batch_matches_tv);
    } else {
        // A listed tape that now matches must leave kBatchDiffersFromTv.
        CHECK(!batch_matches_tv);
    }
    if (batch_matches_tv) ++totals.tv_matched;

    // Forward: one warmup bar, every later bar pushed.
    const Snapshot forward = forward_replay<Host>(c, bars, 1);
    const auto refused = tables.refusals.find(key);
    if (refused != tables.refusals.end()) {
        CHECK(!forward.error.empty());
        CHECK(forward.error.find(refused->second->error_fragment) != std::string::npos);
        std::printf("REFUSED %s [%s] %s (%s)\n", key.c_str(), sample.covers,
                    forward.error.c_str(), refused->second->reason);
        return;
    }
    ++totals.streamed;
    const std::string forward_batch = replay_difference(batch, forward);
    CHECK(forward_batch.empty());
    const std::string forward_tv = batch_matches_tv ? forward_tv_difference(tv, forward, c)
                                                    : std::string("(batch differs from TV)");
    if (batch_matches_tv) CHECK(forward_tv.empty());

    // Every hand-over point.
    std::string first_split;
    std::size_t identical = 0;
    for (std::size_t warmup = 2; warmup < bars.size(); ++warmup) {
        const std::string difference = replay_difference(batch, forward_replay<Host>(c, bars, warmup));
        if (difference.empty()) {
            ++identical;
        } else if (first_split.empty()) {
            first_split = "warmup " + std::to_string(warmup) + ": " + difference;
        }
    }
    const std::size_t splits = bars.size() - 2;
    CHECK(identical == splits);
    totals.splits_identical += identical;
    totals.splits_total += splits;

    // Every rule off: the identity holds there too, and the rules move the
    // forward trades exactly when they move the batch trades.
    switches = every_rule<MarginRuleSwitches>(false);
    const Snapshot batch_off = batch_replay<Host>(c, bars);
    const Snapshot forward_off = forward_replay<Host>(c, bars, 1);
    CHECK(batch_off.error.empty());
    const std::string off_difference = replay_difference(batch_off, forward_off);
    const auto off_listed = tables.off_divergences.find(key);
    const bool batch_moves = !replay_difference(batch, batch_off).empty();
    const bool forward_moves = !replay_difference(forward, forward_off).empty();
    if (off_listed == tables.off_divergences.end()) {
        CHECK(off_difference.empty());
        CHECK(batch_moves == forward_moves);
    } else {
        // A listed tape whose modes now agree must leave kRulesOffModeDivergences.
        CHECK(!off_difference.empty());
        std::printf("  rules-off modes differ (listed): %s\n", off_listed->second.c_str());
    }
    if (forward_moves) ++totals.rules_move_trades;

    // The shipped switch set: the identity on the sample and on every
    // hand-over point, and batch == TradingView unless listed.
    switches = MarginRuleSwitches{};
    const Snapshot batch_shipped = batch_replay<Host>(c, bars);
    CHECK(batch_shipped.error.empty());
    const std::string shipped_tv = batch_tv_difference(tv, batch_shipped, c);
    const auto shipped_listed = tables.shipped_differs_from_tv.find(key);
    if (shipped_listed == tables.shipped_differs_from_tv.end()) {
        CHECK(shipped_tv.empty());
    } else {
        // A listed tape that now matches must leave kShippedDiffersFromTv.
        CHECK(!shipped_tv.empty());
    }
    if (shipped_tv.empty()) ++totals.shipped_tv_matched;
    const std::string shipped_difference =
        replay_difference(batch_shipped, forward_replay<Host>(c, bars, 1));
    CHECK(shipped_difference.empty());
    std::size_t shipped_identical = 0;
    for (std::size_t warmup = 2; warmup < bars.size(); ++warmup) {
        if (replay_difference(batch_shipped, forward_replay<Host>(c, bars, warmup)).empty())
            ++shipped_identical;
    }
    CHECK(shipped_identical == splits);
    ++totals.shipped_streamed;
    totals.shipped_splits_identical += shipped_identical;
    totals.shipped_splits_total += splits;
    std::printf("SHIPPED %s batch==tv:%s forward==batch:%s splits %zu/%zu\n", key.c_str(),
                shipped_tv.empty() ? "yes" : shipped_tv.c_str(),
                shipped_difference.empty() ? "yes" : shipped_difference.c_str(),
                shipped_identical, splits);
    switches = every_rule<MarginRuleSwitches>(true);

    const int entry_bar = !forward.closed.empty() ? forward.closed.front().entry_bar_index
                          : !forward.open.empty() ? forward.open.front().bar_index
                                                  : -1;
    std::printf("%s %s [%s] closed=%zu open=%zu first-entry-bar=%d batch==tv:%s "
                "forward==batch:%s forward==tv:%s splits %zu/%zu rules-off moves trades:%s "
                "off forward==batch:%s forward range-end rows=%zu batch range-end rows=%zu\n",
                forward_batch.empty() && identical == splits && off_difference.empty() ? "SAME"
                                                                                       : "DIFF",
                key.c_str(), sample.covers, forward.closed.size(), forward.open.size(),
                entry_bar, batch_matches_tv ? "yes" : batch_tv.c_str(),
                forward_batch.empty() ? "yes" : forward_batch.c_str(),
                forward_tv.empty() ? "yes" : forward_tv.c_str(), identical, splits,
                forward_moves ? "yes" : "no",
                off_difference.empty() ? "yes" : off_difference.c_str(),
                forward.range_end.size(), batch.range_end.size());
    if (!first_split.empty()) std::printf("  first differing split: %s\n", first_split.c_str());
    if (!forward_batch.empty()) {
        dump("batch", batch);
        dump("forward", forward);
    }
    if (!off_difference.empty()) {
        dump("batch-off", batch_off);
        dump("forward-off", forward_off);
    }
    if (listed != tables.differs_from_tv.end())
        std::printf("  batch differs from TradingView today: %s\n", listed->second.c_str());
}

}  // namespace

int main() {
    std::printf("=== margin_rules_forward_replay: one engine, two modes, on TradingView's tapes ===\n");
    auto& switches = pineforge::source::detail::margin_rule_switches();
    const MarginRuleSwitches tree_defaults = switches;

    Tables tables;
    for (const auto& [key, reason] : kBatchDiffersFromTv) {
        CHECK(!reason.empty());
        CHECK(tables.differs_from_tv.emplace(key, reason).second);
    }
    for (const auto& [key, reason] : kRulesOffModeDivergences) {
        CHECK(!reason.empty());
        CHECK(tables.off_divergences.emplace(key, reason).second);
    }
    for (const auto& [key, reason] : kShippedDiffersFromTv) {
        CHECK(!reason.empty());
        CHECK(tables.shipped_differs_from_tv.emplace(key, reason).second);
    }
    for (const Refusal& refusal : kStreamRefusals) {
        CHECK(std::string(refusal.reason).size() > 0);
        CHECK(tables.refusals.emplace(refusal.key, &refusal).second);
    }

    std::map<std::string, std::vector<Case>> rule_fixtures;
    std::map<std::string, std::vector<LedgerCase>> ledger_fixtures;
    std::map<std::string, std::vector<Bar>> feeds;
    auto feed = [&](const std::string& name, const std::string& file) -> const std::vector<Bar>& {
        const std::string feed_key = name + "/" + file;
        auto found = feeds.find(feed_key);
        if (found == feeds.end()) found = feeds.emplace(feed_key, load_bars(name, file)).first;
        return found->second;
    };
    std::set<std::string> sampled;
    Totals totals;

    for (const Sample& sample : kSamples) {
        const std::string key = std::string(sample.fixture) + "/" + sample.path;
        CHECK(sampled.insert(key).second);
        if (std::string(sample.fixture) == kLedgerFixture) {
            auto loaded = ledger_fixtures.find(sample.fixture);
            if (loaded == ledger_fixtures.end())
                loaded = ledger_fixtures.emplace(sample.fixture, load_ledger_cases(sample.fixture)).first;
            const auto found = std::find_if(loaded->second.begin(), loaded->second.end(),
                                            [&](const LedgerCase& c) { return c.path == sample.path; });
            CHECK(found != loaded->second.end());
            if (found == loaded->second.end()) {
                std::printf("MISSING %s\n", key.c_str());
                continue;
            }
            replay_sample<LedgerHost>(sample, *found, feed(found->fixture, found->bars), tables,
                                      totals);
        } else {
            auto loaded = rule_fixtures.find(sample.fixture);
            if (loaded == rule_fixtures.end())
                loaded = rule_fixtures.emplace(sample.fixture, load_cases(sample.fixture)).first;
            const auto found = std::find_if(loaded->second.begin(), loaded->second.end(),
                                            [&](const Case& c) { return c.path == sample.path; });
            CHECK(found != loaded->second.end());
            if (found == loaded->second.end()) {
                std::printf("MISSING %s\n", key.c_str());
                continue;
            }
            const std::vector<Bar>& bars = feed(found->fixture, found->bars);
            CHECK(!bars.empty() && bars.back().timestamp <= found->to_ms);
            replay_sample<RuleHost>(sample, *found, bars, tables, totals);
        }
    }
    for (const auto& [key, reason] : tables.differs_from_tv) CHECK(sampled.count(key) == 1);
    for (const auto& [key, refusal] : tables.refusals) CHECK(sampled.count(key) == 1);
    for (const auto& [key, reason] : tables.off_divergences) CHECK(sampled.count(key) == 1);
    for (const auto& [key, reason] : tables.shipped_differs_from_tv) CHECK(sampled.count(key) == 1);

    switches = tree_defaults;
    CHECK(same_switches(switches, tree_defaults));
    std::printf("margin_rules_forward_replay: %zu samples, %zu streamed, %zu batch==tv, "
                "splits %zu/%zu identical, rules move trades on %zu\n",
                kSamples.size(), totals.streamed, totals.tv_matched, totals.splits_identical,
                totals.splits_total, totals.rules_move_trades);
    CHECK(totals.streamed + kStreamRefusals.size() == kSamples.size());
    std::printf("shipped switches: %zu streamed, %zu batch==tv, splits %zu/%zu identical\n",
                totals.shipped_streamed, totals.shipped_tv_matched,
                totals.shipped_splits_identical, totals.shipped_splits_total);
    CHECK(totals.shipped_streamed == totals.streamed);
    CHECK(totals.shipped_splits_identical == totals.shipped_splits_total);
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
