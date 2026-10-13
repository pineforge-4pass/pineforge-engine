/*
 * test_point_order_open_call.cpp -- the point order's margin call at a gapped
 * open, using independently designed fifteen-minute bars.
 *
 * Pinned against TradingView behaviour (recorded tapes are kept outside this
 * repository). Queued calls execute before stops when the call still covers
 * the new open's shortfall. With sufficient capital, the gapped stop takes
 * the entire position instead. A partial stop gets priority only if its
 * remainder is margined: a 50 % stop does, a 2 % stop does not.
 *
 * Every row checks side, quantity, entry and exit prices and times, and the
 * exit id. Both partial-stop scenarios also compare batch and forward runs.
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

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

constexpr std::int64_t kEpoch = 946684800000LL;
constexpr std::int64_t kPeriod = 900000LL;

std::int64_t at(int index) { return kEpoch + index * kPeriod; }

std::vector<Bar> load_bars(const std::string& name) {
    // Flat OHLC makes every adverse checkpoint explicit; no hidden extreme
    // changes the call size. The gap opens beyond the resting stop.
    if (name == "close-call") {
        return {{200.0, 200.0, 200.0, 200.0, 1.0, at(0)},
                {201.0, 201.0, 201.0, 201.0, 1.0, at(1)},
                {201.0, 201.0, 201.0, 201.0, 1.0, at(2)},
                {201.0, 201.0, 201.0, 201.0, 1.0, at(3)}};
    }
    return {{100.0, 100.0, 100.0, 100.0, 1.0, at(0)},
            {100.0, 100.0, 100.0, 100.0, 1.0, at(1)},
            {100.1, 100.1, 100.1, 100.1, 1.0, at(2)},
            {103.0, 103.0, 103.0, 103.0, 1.0, at(3)},
            {103.0, 103.0, 103.0, 103.0, 1.0, at(4)},
            {104.0, 104.0, 104.0, 104.0, 1.0, at(5)}};
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
    // Short entry = 200 - 2*0.01 = 199.98; entry fee =
    // 3*199.98*0.0005 = 0.29997. At the signal close, equity =
    // 600.1 - 0.29997 - 3*0.02 = 599.74003 against margin 600:
    // q_restore = 0.25997/200 < 0.01, hence the one-unit queued call.
    // At the gap, q_restore = (603 - 597.04003)/201 = 0.02965...,
    // floor-to-0.01 then 4x = 0.08 <= the queued one unit: no re-sizing.
    // Both gap exits buy at 201 + 2*0.01 = 201.02.
    const Scenario call_sized_at_close{"close-sized call, then the gapped stop",
        "close-call", 0.01, 0.01, 600.1, false, 3.0, 0.05, 2, true,
        at(0), 200.5, 100.0, at(2)};
    expect(call_sized_at_close, {{1.0, at(0), at(1), 19998, 20102, "__margin_call__"},
                                 {2.0, at(0), at(1), 19998, 20102, "SE"}}, false);
    // Extra capital removes the close's deficit, so all three hit the stop.
    const Scenario room_at_close{"control: no call sized at the close, the stop takes all",
        "close-call", 0.01, 0.01, 650.1, false, 3.0, 0.05, 2, true,
        at(0), 200.5, 100.0, at(2)};
    expect(room_at_close, {{3.0, at(0), at(1), 19998, 20102, "SE"}}, false);

    // Equity 100000 / signal 100 = 1000 whole units. At 100.1:
    // q_restore = 1000 - 99900/100.1 = 1.998... -> floor 1 * 4 = 4.
    // At gap 103 the 996 survivors have equity
    // 100000 - 4*0.1 - 996*3 = 97011.6; q_restore =
    // 996 - 97011.6/103 = 54.1398... -> floor 54 * 4 = 216.
    // A 2 % stop reserves floor(1000*0.02)=20, leaving 976 if first:
    // 976*103 > 97011.6, so the call precedes it; 996-216-20=760 remain.
    // A 50 % stop reserves 500, leaving 496*103=51088 < 97011.6:
    // it fills first with no gap call, and 996-500=496 remain.
    // The sub-tick stop is crossed by the gap; prices/0.01 give tick rows.
    const Scenario partial_short_remainder{"2 % stop: the remainder is short, the call comes first",
        "partial-call", 0.01, 1.0, 100000.0, true, 0.0, 0.0, 0, false,
        at(0), 100.505, 2.0, at(4)};
    expect(partial_short_remainder, {{4.0, at(1), at(2), 10000, 10010, "__margin_call__"},
                                     {216.0, at(1), at(3), 10000, 10300, "__margin_call__"},
                                     {20.0, at(1), at(3), 10000, 10300, "SE"},
                                     {760.0, at(1), at(5), 10000, 10400, "__close__"}}, true);
    const Scenario partial_margined_remainder{"50 % stop: the remainder is margined, the stop fills first",
        "partial-call", 0.01, 1.0, 100000.0, true, 0.0, 0.0, 0, false,
        at(0), 100.505, 50.0, at(4)};
    expect(partial_margined_remainder, {{4.0, at(1), at(2), 10000, 10010, "__margin_call__"},
                                        {500.0, at(1), at(3), 10000, 10300, "SE"},
                                        {496.0, at(1), at(5), 10000, 10400, "__close__"}}, true);

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
