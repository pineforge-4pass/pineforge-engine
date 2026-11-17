// TradingView's exit reservation rule, on constructed bars:
//
//   A working exit order reserves its quantity of the open position. A new
//   exit's quantity, whether fixed or a qty_percent of the whole open
//   position, is capped at the quantity other working exits have not
//   reserved. An exit left nothing does not become active and never fills.
//
// Rows and where their expectations come from. Each captured row is the
// TradingView capture's order sequence replayed at this file's own prices;
// the quantities, the exit that fills and the bar it fills on are the
// capture's (answers digest in brackets).
//
// 1. stop-alone [d82b3e63]: a qty-2 entry and, two bars after it opened, one
//    qty-2 stop exit. Nothing else reserves the position: the stop fills 2.
// 2. holder-and-stop [425bfb85]: a qty-2 entry, a qty-1 exit at a far stop,
//    then a qty-1 stop exit of another id. One unit is unreserved: the stop
//    fills 1, and the far exit's unit stays until the script closes it.
// 3. resize-75 [59a74e12], with one and with two pyramiding slots: a 4-unit
//    entry, a qty-2 exit at a far stop placed first, and a 50% limit exit at
//    a far level behind it; that limit exit is re-issued at a reachable level
//    with qty_percent 75. 75% of the whole position is 3, 2 are unreserved:
//    it fills 2.
// 4. resize-25 [f3a3ee15]: the same, re-issued with qty_percent 25. 25% of
//    the whole position is 1 (not 25% of the 2 unreserved units): it fills 1.
// 5. add-on-chain: the order sequence of a strategy reported against its
//    TradingView trade list (OANDA:XAUUSD 15m; not a captured cell). An entry
//    "Long" of 2 with two qty-1 exits, and an add-on "LongAdd" of 2 with two
//    qty-1 exits of its own, each exit placed once with its entry. The first
//    add-on's first exit fills at its limit; its second exit keeps working
//    while later "Long" entries' exits close the add-on's last lot first in
//    first out. A second add-on is then placed with the same two exit ids.
//    TradingView fills three exits when its stop is crossed: the first
//    add-on's still-working second exit at the new level, and both exits of
//    the new add-on; the book is flat. The next "Long" and the next add-on
//    then each close through their own two exits. Before this rule was
//    applied to an exit that names its quantity in units, the new add-on's
//    first exit was refused as a partial exit behind a full one (the older
//    exit's one unit was all its entry held when it was last sized), one unit
//    stayed open, and the later exits no longer matched the trade list.
//
// No market data or fixture is read.

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace pineforge;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1'735'689'600'000LL;
constexpr std::int64_t kInterval = 15 * 60'000LL;
// strategy_close_all()'s report label, not a strategy.exit.
constexpr const char* kCloseAll = "__close__";

struct Ohlc { double open, high, low, close; };

std::vector<Bar> bars_of(const std::vector<Ohlc>& rows) {
    std::vector<Bar> bars;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        bars.push_back({rows[i].open, rows[i].high, rows[i].low, rows[i].close, 1000.0,
                        kStart + static_cast<std::int64_t>(i) * kInterval});
    }
    return bars;
}

struct Setup {
    int pyramiding = 1;
    bool close_any = false;
    double margin = 100.0;
};

class Probe final : public source::PineStrategyHost {
public:
    using Script = std::function<void(Probe&, int)>;

    Probe(const Setup& setup, Script script) : script_(std::move(script)) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = false;
        cfg.process_orders_on_close = false;
        cfg.pyramiding = setup.pyramiding;
        cfg.initial_capital = 1'000'000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.margin_long = cfg.margin_short = setup.margin;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        cfg.close_entries_rule_any = setup.close_any;
        configure_pine_strategy(cfg);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(1.0);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", 1.0);
    }

    void on_source_bar(const Bar&) override { script_(*this, pine_bar_index()); }

    void entry(const char* id, double qty) { strategy_entry(id, true, kNaN, kNaN, qty); }
    void exit_units(const char* id, const char* from, double limit, double stop, double qty) {
        strategy_exit(id, from, limit, stop, kNaN, kNaN, kNaN, 100.0, {}, qty);
    }
    void exit_percent(const char* id, const char* from, double limit, double percent) {
        strategy_exit(id, from, limit, kNaN, kNaN, kNaN, kNaN, percent);
    }
    void close_all() { strategy_close_all(); }
    double position() const { return signed_position_size(); }

private:
    Script script_;
};

// One closed trade row: the bar it closed on, the entry it belonged to, the
// exit that closed it, its quantity and its exit price.
using Fill = std::tuple<int, std::string, std::string, double, double>;

struct Row {
    const char* name;
    Setup setup;
    std::vector<Ohlc> tape;
    Probe::Script script;
    std::vector<Fill> expected;
};

std::string text(const Fill& fill) {
    char line[160];
    std::snprintf(line, sizeof line, "bar %d entry=%s exit=%s quantity=%.17g price=%.17g",
                  std::get<0>(fill), std::get<1>(fill).c_str(), std::get<2>(fill).c_str(),
                  std::get<3>(fill), std::get<4>(fill));
    return line;
}

bool run_row(const Row& row) {
    Probe probe(row.setup, row.script);
    const auto bars = bars_of(row.tape);
    probe.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    std::vector<Fill> actual;
    bool open_row = false;
    std::printf("%s: trades=%d final position=%.17g\n", row.name, probe.trade_count(),
                probe.position());
    for (int i = 0; i < probe.trade_count(); ++i) {
        const Trade& trade = probe.get_trade(i);
        const Fill fill{trade.exit_bar_index, trade.entry_id, trade.exit_id, trade.qty,
                        trade.exit_price};
        std::printf("  %s open=%d\n", text(fill).c_str(), trade.open_at_end ? 1 : 0);
        if (trade.open_at_end) open_row = true;
        else actual.push_back(fill);
    }
    std::fflush(stdout);

    bool ok = true;
    if (!probe.last_error().empty()) {
        std::printf("FAIL %s: run error \"%s\"\n", row.name, probe.last_error().c_str());
        ok = false;
    }
    if (open_row) {
        std::printf("FAIL %s: a trade is still open at the end\n", row.name);
        ok = false;
    }
    if (probe.position() != 0.0) {
        std::printf("FAIL %s: final position actual=%.17g expected=0\n", row.name,
                    probe.position());
        ok = false;
    }
    // The closed rows as a multiset: which exit closed which entry's unit on
    // which bar, at what quantity and price. The order of same-bar rows is not
    // compared.
    std::vector<Fill> expected = row.expected;
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    std::vector<Fill> missing, unexpected;
    std::set_difference(expected.begin(), expected.end(), actual.begin(), actual.end(),
                        std::back_inserter(missing));
    std::set_difference(actual.begin(), actual.end(), expected.begin(), expected.end(),
                        std::back_inserter(unexpected));
    for (const Fill& fill : missing) {
        std::printf("FAIL %s: expected and absent: %s\n", row.name, text(fill).c_str());
        ok = false;
    }
    for (const Fill& fill : unexpected) {
        std::printf("FAIL %s: present and not expected: %s\n", row.name, text(fill).c_str());
        ok = false;
    }
    if (ok) std::printf("PASS %s\n", row.name);
    return ok;
}

// The captured later-stop cells: the entry call at bar 0, a stop at 95 that
// only bar 4 crosses, a far stop at 80 that no bar reaches, close_all at bar 5.
const std::vector<Ohlc> kStopTape{
    {100, 101, 99, 100}, {100, 101, 99, 100}, {100, 102, 99, 101}, {101, 102, 100, 101},
    {100, 101, 94, 96}, {97, 99, 96, 98}, {98, 99, 97, 98}, {98, 99, 97, 98}};

// The captured resize cells: a limit at 108 first reached inside bar 4, a far
// stop at 50 and a far limit at 150 that no bar reaches, close_all at bar 9.
const std::vector<Ohlc> kResizeTape{
    {100, 101, 99, 100}, {100, 101, 99, 100}, {100, 102, 99, 101}, {101, 103, 100, 102},
    {102, 109, 101, 105}, {105, 107, 104, 106}, {106, 107, 105, 106}, {106, 107, 105, 106},
    {106, 107, 105, 106}, {106, 107, 105, 106}, {106, 107, 105, 106}, {106, 107, 105, 106}};

// The add-on chain. Every stop a bar crosses is crossed after the bar opened
// above it, so it books the level; the two limits of bar 7 are passed at the
// open and book the open.
const std::vector<Ohlc> kChainTape{
    {100, 101, 99, 100},    // 0  Long and its exits (stop 95)
    {100, 101, 99, 100},    // 1  Long opens at 100
    {100, 101, 94, 97},     // 2  both stops at 95; LongAdd and its exits
    {97, 99, 96, 98},       // 3  LongAdd opens at 97
    {98, 106, 97, 105},     // 4  LongAddT1's limit at 105
    {105, 109, 104, 108},   // 5  Long and its exits (limits 112 and 114)
    {108, 109, 107, 108},   // 6  Long opens at 108
    {116, 117, 115, 116},   // 7  both limits at the open, 116
    {116, 119, 115, 118},   // 8  Long and its exits (stop 113)
    {118, 119, 117, 118},   // 9  Long opens at 118
    {118, 119, 112, 115},   // 10 both stops at 113; LongAdd and its exits (stop 113)
    {115, 116, 112, 114},   // 11 LongAdd opens at 115, the bar crosses 113
    {114, 121, 114, 120},   // 12 Long and its exits (stop 117)
    {120, 121, 116, 118},   // 13 Long opens at 120, the bar crosses 117; LongAdd and its exits
    {118, 119, 118, 118},   // 14 LongAdd opens at 118
    {118, 119, 118, 118},   // 15
    {118, 119, 118, 118},   // 16
    {118, 119, 116, 117},   // 17 the bar crosses 117
    {118, 119, 118, 118},   // 18
    {118, 119, 118, 118}};  // 19

void chain(Probe& probe, int bar) {
    const auto long_entry = [&](double first, double second, double stop) {
        probe.entry("Long", 2);
        probe.exit_units("LongT1", "Long", first, stop, 1);
        probe.exit_units("LongT2", "Long", second, stop, 1);
    };
    const auto add_on = [&](double first, double second, double stop) {
        probe.entry("LongAdd", 2);
        probe.exit_units("LongAddT1", "LongAdd", first, stop, 1);
        probe.exit_units("LongAddT2", "LongAdd", second, stop, 1);
    };
    if (bar == 0) long_entry(110, 120, 95);
    if (bar == 2) add_on(105, 150, 95);
    if (bar == 5) long_entry(112, 114, 100);
    if (bar == 8) long_entry(125, 128, 113);
    if (bar == 10) add_on(122, 126, 113);
    if (bar == 12) long_entry(128, 131, 117);
    if (bar == 13) add_on(126, 129, 117);
}

Probe::Script later_stop(double holder_qty, double stop_qty) {
    return [=](Probe& probe, int bar) {
        if (bar == 0) probe.entry("entry", 2);
        if (bar == 1 && holder_qty > 0.0)
            probe.exit_units("holder", "entry", kNaN, 80, holder_qty);
        if (bar == 3) probe.exit_units("later", "entry", kNaN, 95, stop_qty);
        if (bar == 5) probe.close_all();
    };
}

Probe::Script resize(double percent) {
    return [=](Probe& probe, int bar) {
        if (bar == 0) probe.entry("entry", 4);
        if (bar == 1) {
            probe.exit_units("earlier", "entry", kNaN, 50, 2);
            probe.exit_percent("resize", "entry", 150, 50);
        }
        if (bar == 3) probe.exit_percent("resize", "entry", 108, percent);
        if (bar == 9) probe.close_all();
    };
}

std::vector<Row> rows() {
    Setup capture{};
    Setup resize_one{};
    resize_one.close_any = true;
    resize_one.margin = 0.0;
    Setup resize_two = resize_one;
    resize_two.pyramiding = 2;
    Setup two_slots{};
    two_slots.pyramiding = 2;
    return {
        {"stop-alone", capture, kStopTape, later_stop(0, 2),
         {Fill{4, "entry", "later", 2, 95}}},
        {"holder-and-stop", capture, kStopTape, later_stop(1, 1),
         {Fill{4, "entry", "later", 1, 95}, Fill{6, "entry", kCloseAll, 1, 98}}},
        {"resize-75", resize_one, kResizeTape, resize(75),
         {Fill{4, "entry", "resize", 2, 108}, Fill{10, "entry", kCloseAll, 2, 106}}},
        {"resize-75-two-slots", resize_two, kResizeTape, resize(75),
         {Fill{4, "entry", "resize", 2, 108}, Fill{10, "entry", kCloseAll, 2, 106}}},
        {"resize-25", resize_one, kResizeTape, resize(25),
         {Fill{4, "entry", "resize", 1, 108}, Fill{10, "entry", kCloseAll, 3, 106}}},
        {"add-on-chain", two_slots, kChainTape, chain,
         {Fill{2, "Long", "LongT1", 1, 95}, Fill{2, "Long", "LongT2", 1, 95},
          Fill{4, "LongAdd", "LongAddT1", 1, 105},
          // First in first out: the first add-on's last lot, then a Long lot.
          Fill{7, "LongAdd", "LongT1", 1, 116}, Fill{7, "Long", "LongT2", 1, 116},
          Fill{10, "Long", "LongT1", 1, 113}, Fill{10, "Long", "LongT2", 1, 113},
          // The three exits of the second add-on's entry bar: the first
          // add-on's standing exit takes the oldest lot, a Long one.
          Fill{11, "Long", "LongAddT2", 1, 113}, Fill{11, "LongAdd", "LongAddT1", 1, 113},
          Fill{11, "LongAdd", "LongAddT2", 1, 113},
          Fill{13, "Long", "LongT1", 1, 117}, Fill{13, "Long", "LongT2", 1, 117},
          Fill{17, "LongAdd", "LongAddT1", 1, 117}, Fill{17, "LongAdd", "LongAddT2", 1, 117}}},
    };
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::printf("Expected zero arguments or one row name.\n");
        return 1;
    }
    int selected = 0;
    int failures = 0;
    for (const Row& row : rows()) {
        if (argc == 2 && std::string(argv[1]) != row.name) continue;
        ++selected;
        if (!run_row(row)) ++failures;
    }
    if (selected == 0) {
        std::printf("Unknown row: %s\n", argv[1]);
        return 1;
    }
    std::printf("%d rows, %d failures\n", selected, failures);
    return failures == 0 ? 0 : 1;
}
