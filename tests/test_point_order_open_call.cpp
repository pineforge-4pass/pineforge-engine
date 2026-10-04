/*
 * test_point_order_open_call.cpp -- the point order's margin call at a gapped
 * open, on the bars of tests/fixtures/admission_rules.
 *
 * At a bar's open TradingView executes the queued orders first, then the
 * exits the open reaches, and checks the margin on what is left. Two shapes
 * keep the earlier order, and a control of each fills the exit first:
 *
 * - A process_orders_on_close short that the fee and the slippage leave short
 *   of margin at its entry close carries a call sized at that close. While
 *   that call still covers the open's own shortfall the open does not re-size
 *   it, and it is a queued order: it executes at the next open before the stop
 *   the open gaps through. This is TradingView's tape of
 *   axealgo-tp-sl-toolkit-axealgo on OANDA:XAUUSD 15 (2025-06-01 22:00: Margin
 *   call 1, then the stop's 1.62); here an explicit short of 3, at a capital
 *   one dollar over its cost at the signal close, does the same on the XAUUSD
 *   fixture bars.
 *   - Control, with $500 more capital (10509.845 against 10010.845): no call
 *     is sized at the close, and the gapped stop takes the whole short.
 * - A partial stop the open gaps through fills first only when what it leaves
 *   is margined.
 *   - The 50 % stop leaves a margined half and fills first with no call, as
 *     stop_priority stop-09 and stop-10 do on TradingView's tapes.
 *   - The 2 % stop leaves a short remainder. Then the call at the open comes
 *     first, as before. No TradingView tape decides that case yet.
 *
 * Every row below is asserted as the engine books it: side, quantity, entry
 * and exit price and time, and whether a margin call or the stop closed it.
 */

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_adapter.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
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

#ifndef PINEFORGE_ADMISSION_RULES_FIXTURE_DIR
#error "PINEFORGE_ADMISSION_RULES_FIXTURE_DIR must name tests/fixtures/admission_rules"
#endif

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::vector<Bar> load_bars(const std::string& name) {
    std::ifstream in(std::string(PINEFORGE_ADMISSION_RULES_FIXTURE_DIR) + "/bars/" + name);
    std::string line;
    std::getline(in, line);
    std::vector<Bar> bars;
    while (std::getline(in, line)) {
        std::vector<std::string> cells;
        std::string cell;
        std::istringstream row(line);
        while (std::getline(row, cell, ',')) cells.push_back(cell);
        if (cells.size() != 6) continue;
        bars.push_back({std::strtod(cells[1].c_str(), nullptr), std::strtod(cells[2].c_str(), nullptr),
                        std::strtod(cells[3].c_str(), nullptr), std::strtod(cells[4].c_str(), nullptr),
                        std::strtod(cells[5].c_str(), nullptr),
                        static_cast<std::int64_t>(std::strtoll(cells[0].c_str(), nullptr, 10))});
    }
    return bars;
}

// One short opened on one bar with its protective stop, closed by close_all
// at the end of the window.
struct Scenario {
    const char* name;
    const char* bars;
    double tick;
    double step;
    double capital;
    bool percent_default;  // 100 % of equity; else an explicit quantity
    double explicit_qty;
    double fee_percent;
    int slippage;
    bool pooc;
    std::int64_t open_ms;
    double stop;
    double stop_percent;
    std::int64_t close_all_ms;
};

class ScenarioHost final : public source::PineStrategyHost {
public:
    explicit ScenarioHost(const Scenario& s) : s_(s) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.initial_capital = s.capital;
        config.default_qty_type = static_cast<int>(
            s.percent_default ? QtyType::PERCENT_OF_EQUITY : QtyType::FIXED);
        config.default_qty_value = s.percent_default ? 100.0 : 1.0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = s.fee_percent;
        config.slippage = s.slippage;
        config.process_orders_on_close = s.pooc;
        config.pyramiding = 1;
        configure_pine_strategy(config);
        set_syminfo_mintick(s.tick);
        set_syminfo_metadata("qty_step", s.step);
        if (std::string(s.bars).rfind("f-", 0) == 0) {
            set_syminfo_timezone("America/New_York");
            set_syminfo_session("0930-1600");
        }
    }

    void on_source_bar(const Bar& bar) override {
        if (bar.timestamp == s_.open_ms) {
            strategy_entry("Short", false, kNaN, kNaN, s_.percent_default ? kNaN : s_.explicit_qty);
            strategy_exit("SE", "Short", kNaN, s_.stop, kNaN, kNaN, kNaN, s_.stop_percent);
        }
        if (bar.timestamp == s_.close_all_ms) strategy_close_all();
    }

    double position() const { return signed_position_size(); }

private:
    Scenario s_;
};

struct Row {
    double qty;
    std::int64_t entry_ms;
    std::int64_t exit_ms;
    long entry_ticks;
    long exit_ticks;
    const char* exit_id;
};

long ticks(double price, double tick) { return std::lround(price / tick); }

void expect(const Scenario& s, const std::vector<Row>& rows, bool forward) {
    const std::vector<Bar> bars = load_bars(s.bars);
    CHECK(!bars.empty());
    if (bars.empty()) return;
    ScenarioHost batch(s);
    batch.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    CHECK(batch.last_error().empty());
    std::printf("%s: %d rows\n", s.name, batch.report_trade_count());
    CHECK(batch.report_trade_count() == static_cast<int>(rows.size()));
    for (int i = 0; i < batch.report_trade_count() && i < static_cast<int>(rows.size()); ++i) {
        const Trade& t = batch.get_report_trade(i);
        const Row& want = rows[static_cast<std::size_t>(i)];
        std::printf("  %s %.17g %lld -> %lld %.17g -> %.17g [%s]\n", t.is_long ? "long" : "short",
                    t.qty, static_cast<long long>(t.entry_time), static_cast<long long>(t.exit_time),
                    t.entry_price, t.exit_price, t.exit_id.c_str());
        CHECK(!t.is_long);
        CHECK(std::abs(t.qty - want.qty) < 1e-9);
        CHECK(t.entry_time == want.entry_ms);
        CHECK(t.exit_time == want.exit_ms);
        CHECK(ticks(t.entry_price, s.tick) == want.entry_ticks);
        CHECK(ticks(t.exit_price, s.tick) == want.exit_ticks);
        CHECK(t.exit_id == want.exit_id);
    }
    CHECK(batch.position() == 0.0);
    if (!forward) return;
    // The same rows bar by bar: no rule here waits on a whole series.
    ScenarioHost stream(s);
    bool fed = stream.stream_begin(bars.data(), 1, "15", "15");
    for (std::size_t i = 1; fed && i < bars.size(); ++i) fed = stream.stream_push_bar(bars[i]);
    CHECK(fed);
    CHECK(stream.trade_count() == batch.trade_count());
    for (int i = 0; fed && i < stream.trade_count() && i < batch.trade_count(); ++i) {
        const Trade& a = stream.get_trade(i);
        const Trade& b = batch.get_trade(i);
        CHECK(a.qty == b.qty && a.entry_time == b.entry_time && a.exit_time == b.exit_time
              && a.entry_price == b.entry_price && a.exit_price == b.exit_price);
    }
}

}  // namespace

int main() {
    std::printf("=== the point order's margin call at a gapped open ===\n");
    // XAUUSD fixture bars: an explicit short of 3 at the 2025-07-04 16:45 UTC
    // close under process_orders_on_close (fee 0.05 %, slippage 2), its stop at
    // 3340, gapped by the 3341.57 open of 2025-07-06 22:00.
    const char* xau = "xau-2025-07-03-2025-07-08.csv";
    const std::int64_t xau_open = 1751647500000;
    const std::int64_t xau_gap = 1751839200000;
    const std::int64_t xau_end = 1751931000000;
    const Scenario call_sized_at_close{"close-sized call, then the gapped stop", xau, 0.001, 0.01,
        10010.845, false, 3.0, 0.05, 2, true, xau_open, 3340.0, 100.0, xau_end};
    expect(call_sized_at_close, {{1.0, xau_open, xau_gap, 3336613, 3341572, "__margin_call__"},
                                 {2.0, xau_open, xau_gap, 3336613, 3341572, "SE"}},
           false);
    const Scenario room_at_close{"control: no call sized at the close, the stop takes all", xau,
        0.001, 0.01, 10509.845, false, 3.0, 0.05, 2, true, xau_open, 3340.0, 100.0, xau_end};
    expect(room_at_close, {{3.0, xau_open, xau_gap, 3336613, 3341572, "SE"}}, false);

    // NYSE:F fixture bars: 100 % of equity short at the 2025-10-20 18:45 UTC
    // signal, filled at the next open; a partial stop at 12.115 gapped by the
    // 12.26 open of 2025-10-21 13:30.
    const char* f = "f-2025-10-20-2025-10-22.csv";
    const std::int64_t f_signal = 1760985900000;
    const std::int64_t f_fill = 1760986800000;
    const std::int64_t f_call = 1760989500000;
    const std::int64_t f_gap = 1761053400000;
    const std::int64_t f_end_signal = 1761073200000;
    const std::int64_t f_end = 1761074100000;
    const Scenario partial_short_remainder{"2 % stop: the remainder is short, the call comes first",
        f, 0.01, 1.0, 106282.0, true, 0.0, 0.0, 0, false, f_signal, 12.115, 2.0, f_end_signal};
    expect(partial_short_remainder, {{112.0, f_fill, f_call, 1200, 1202, "__margin_call__"},
                                     {1040.0, f_fill, f_gap, 1200, 1226, "__margin_call__"},
                                     {177.0, f_fill, f_gap, 1200, 1226, "SE"},
                                     {7527.0, f_fill, f_end, 1200, 1257, "__close__"}},
           true);
    const Scenario partial_margined_remainder{"50 % stop: the remainder is margined, the stop fills first",
        f, 0.01, 1.0, 106282.0, true, 0.0, 0.0, 0, false, f_signal, 12.115, 50.0, f_end_signal};
    expect(partial_margined_remainder, {{112.0, f_fill, f_call, 1200, 1202, "__margin_call__"},
                                        {4428.0, f_fill, f_gap, 1200, 1226, "SE"},
                                        {4316.0, f_fill, f_end, 1200, 1257, "__close__"}},
           true);

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
