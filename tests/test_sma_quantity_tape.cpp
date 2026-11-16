// test_sma_quantity_tape -- an order quantity sized from ta.sma is cut to the
// lot TradingView filled, also where the exact quantity is a whole lot.
//
// TradingView rows: the first 18 closed trades of the offset-0 capture of the
// SMA quantity reconstruction, taken 2026-11-16 (BINANCE:BTCUSDT 60, deep
// backtest 2024-01-01..2024-03-01 UTC, quantity step 0.00001; script sha256
// 20aa1262e9b81ca07a94f168b6afbc2ab796aeb6626e6f2872b00438437d822c, trade
// list sha256
// 368dd136315d3660188468c71eae8ca747ba6b46105522ae5b87c36c4dc0982b). The
// script enters on bars 2, 5, 8, ... with
//     qty = ta.sma(close, 3) * 0.001 + offsetSteps * 0.00001
// and closes one bar after the fill. The bars below are the feed's first 56
// (bar 0 = 2024-01-01 00:00 UTC), exactly enough for those 18 trades to open
// and close. The host is tests/sma_quantity_host.hpp, a hand lowering of the
// script, not generated code.
//
// What the rows pin:
//   * five of the 18 exact quantities are whole lots (the three closes sum to
//     a multiple of 3 cents): trades 1, 5, 8, 13 and 18. TradingView fills
//     that lot on four of them. On trade 18 (closes 45262.69, 45183.45,
//     45172.13: exact quantity 45.20609) it fills 45.20608: its average is
//     the correctly rounded 45206.09, the product with 0.001 lands one unit
//     in the last place below 45.20609, and the lot floor never rounds up;
//   * a moving average one unit in the last place higher on that bar gives
//     the product 45.20609 itself and one lot step more: the difference this
//     test was written for;
//   * entry and exit are at the next bar's open in every row.
//
// The -0.25 and +0.25 step forms have no TradingView tape. Their rows are
// exact-arithmetic controls: with the offset every exact quantity is at
// least 1/12 of a step away from a lot boundary, so the lot is the floor of
// the exact quantity whatever the last place of the average is.
#include "sma_quantity_host.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using namespace sma_quantity;

struct FeedBar {
    double open, high, low, close, volume;
};

const FeedBar kFeed[] = {
    {42283.58, 42554.57, 42261.02, 42475.23, 1271.68108},
    {42475.23, 42775, 42431.65, 42613.56, 1196.37856},
    {42613.57, 42638.41, 42500, 42581.1, 685.2198},
    {42581.09, 42586.64, 42230.08, 42330.49, 794.80391},
    {42330.5, 42399.99, 42209.46, 42399.99, 715.4176},
    {42399.98, 42406, 42180.77, 42234.01, 736.53152},
    {42234.01, 42424.82, 42208.68, 42396.69, 601.3725},
    {42396.69, 42500, 42396.68, 42492.46, 653.67718},
    {42492.46, 42556.5, 42452.58, 42549.99, 427.19365},
    {42550, 42699.32, 42537.18, 42649.69, 879.62491},
    {42649.69, 42749.36, 42628, 42691.1, 856.81851},
    {42691.1, 42762.39, 42605.21, 42690.2, 785.16567},
    {42690.21, 42780, 42611.45, 42648.38, 785.52851},
    {42648.37, 42748, 42612.76, 42715.54, 497.2024},
    {42715.54, 42715.55, 42580, 42612.24, 711.01152},
    {42612.24, 42847.07, 42586.51, 42783.05, 942.70163},
    {42783.05, 42856, 42664.42, 42725.15, 918.71056},
    {42725.15, 42824.9, 42704.42, 42812.7, 677.33678},
    {42812.71, 43208.86, 42812.7, 43111.1, 2143.30897},
    {43111.1, 43550, 43103.64, 43517.99, 1947.61533},
    {43517.98, 43829.99, 43510.71, 43687.86, 2905.07295},
    {43687.85, 43792.59, 43480.76, 43619.21, 1541.59126},
    {43619.22, 43661.1, 43361.6, 43529.93, 1024.36372},
    {43529.94, 44184.1, 43529.93, 44179.55, 3475.97051},
    {44179.55, 45200, 44148.34, 45056, 6986.25967},
    {45056, 45372.79, 44686, 44855.96, 4397.86376},
    {44855.96, 45381.42, 44855.95, 45378.96, 3016.04235},
    {45378.96, 45500, 45131.1, 45402.38, 2693.12081},
    {45402.38, 45468.99, 45170, 45179.8, 2268.27572},
    {45179.8, 45324.72, 45121.96, 45179.48, 1617.2842},
    {45179.48, 45308.15, 45130, 45168.75, 1390.33832},
    {45168.75, 45643.75, 45168.74, 45455.53, 3112.52478},
    {45455.52, 45852.93, 45326.6, 45759.3, 3323.21707},
    {45759.3, 45879.63, 45631.1, 45673.48, 2096.46195},
    {45673.48, 45711.1, 45431.52, 45584.99, 2069.22249},
    {45585, 45698.67, 45200, 45433.99, 2538.49307},
    {45433.99, 45589.68, 45271.04, 45301.99, 2128.11147},
    {45302, 45705.8, 45264.39, 45637.93, 2253.76868},
    {45637.92, 45800, 44970, 45258.73, 4553.21243},
    {45258.73, 45349.98, 44833.33, 45184.82, 4301.03657},
    {45184.83, 45399.84, 44890.11, 45357.84, 3052.23783},
    {45357.84, 45382.08, 44950.48, 44966.15, 2540.00308},
    {44966.16, 45271.2, 44944, 45154.69, 1806.0773},
    {45154.7, 45243, 44790.96, 45039.49, 2347.28624},
    {45039.49, 45180.55, 44742.96, 44788.28, 1940.02489},
    {44788.27, 45119.76, 44600, 45119.74, 2043.74183},
    {45119.74, 45297.26, 44840.01, 44981.73, 1591.57491},
    {44981.72, 45049.98, 44802.01, 44946.91, 1080.22719},
    {44946.91, 45212.57, 44862.11, 45135.61, 1410.31904},
    {45135.61, 45329.83, 45102.72, 45317.58, 1166.18095},
    {45317.58, 45410.91, 45211.73, 45274.57, 1434.81631},
    {45274.57, 45310, 45151.91, 45262.69, 739.36966},
    {45262.7, 45272.16, 45115.59, 45183.45, 796.786},
    {45183.45, 45294.11, 45118.91, 45172.13, 849.38832},
    {45172.12, 45363.19, 45160, 45326.93, 1264.55564},
    {45326.92, 45384.84, 44940.93, 45011.77, 1780.38521},
};
constexpr int kFeedBars = static_cast<int>(sizeof(kFeed) / sizeof(kFeed[0]));

struct TapeTrade {
    int command;          // the bar whose close placed the entry
    long long lots;       // Size (qty) in steps of 0.00001
    double entry_price;   // Price USDT of the entry row
    double exit_price;    // Price USDT of the exit row
};

// TradingView's List of Trades, trades 1..18 of the offset-0 capture.
const TapeTrade kTape[] = {
    {2, 4255663, 42581.09, 42330.5},   {5, 4232149, 42234.01, 42396.69},
    {8, 4247971, 42550, 42649.69},     {11, 4267699, 42690.21, 42648.37},
    {14, 4265872, 42612.24, 42783.05}, {17, 4277363, 42812.71, 43111.1},
    {20, 4343898, 43687.85, 43619.22}, {23, 4377623, 44179.55, 45056},
    {26, 4509697, 45378.96, 45402.38}, {29, 4525388, 45179.48, 45168.75},
    {32, 4546119, 45759.3, 45673.48},  {35, 4556415, 45433.99, 45302},
    {38, 4539955, 45258.73, 45184.83}, {41, 4516960, 44966.16, 45154.7},
    {44, 4499415, 44788.27, 45119.74}, {47, 4501612, 44946.91, 45135.61},
    {50, 4524258, 45274.57, 45262.7},  {53, 4520608, 45172.12, 45326.92},
};
constexpr int kTapeTrades = static_cast<int>(sizeof(kTape) / sizeof(kTape[0]));
// The one row whose whole-lot exact quantity TradingView fills one step under.
constexpr int kStepUnderTrade = 18;

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (int local = 0; local < kFeedBars; ++local) {
        const FeedBar& row = kFeed[local];
        bars.push_back({row.open, row.high, row.low, row.close, row.volume,
                        first_time + local * hour});
    }
    return bars;
}

// The closes of a command bar and of the two bars before it, in cents: the
// exact quantity of the offset-0 form is this many thirds of a lot step.
long long window_cents(int command) {
    long long cents = 0;
    for (int bar = command - 2; bar <= command; ++bar)
        cents += std::llround(kFeed[bar].close * 100.0);
    return cents;
}

// A fill as a whole number of lot steps. The fill must sit on the lot grid
// up to the last places of its own double; the export's five decimals cannot
// tell further than that.
long long filled_lots(const Trade& trade, const std::string& label) {
    const long long lots = std::llround(trade.qty / quantity_step);
    require(std::abs(trade.qty - static_cast<double>(lots) * quantity_step)
                < quantity_step * 1e-6,
            label + " fill is off the lot grid: " + word(trade.qty));
    return lots;
}

void run_feed(SmaQuantityHost& host, const std::string& form) {
    const auto bars = feed();
    host.run(bars.data(), static_cast<int>(bars.size()), "60", "60", false);
    require(host.last_run_status() == 0 && host.last_error().empty(),
            form + " runtime failed: " + host.last_error());
    require(host.closed_trade_count() == static_cast<std::size_t>(kTapeTrades)
                && host.report_trade_count() == kTapeTrades
                && host.pending_order_count() == 0 && host.live_position_size() == 0,
            form + " missing, rejected, extra or open trades");
}

void require_schedule(const Trade& trade, int command, const std::string& label) {
    require(trade.is_long && !trade.open_at_end && trade.entry_id == "L",
            label + " wrong trade identity");
    require(trade.entry_time == first_time + (command + 1) * hour
                && trade.exit_time == first_time + (command + 2) * hour,
            label + " changed market schedule");
    // Half a tick: the fill is the next bar's open or it is a different price.
    require(std::abs(trade.entry_price - kFeed[command + 1].open) < 0.005
                && std::abs(trade.exit_price - kFeed[command + 2].open) < 0.005,
            label + " changed fill price");
}

void tradingview_rows() {
    SmaQuantityHost host(0.0);
    run_feed(host, "offset 0");
    int whole_lot_rows = 0;
    for (int i = 0; i < kTapeTrades; ++i) {
        const TapeTrade& tv = kTape[i];
        const std::string label = "trade " + std::to_string(i + 1)
            + " (command bar " + std::to_string(tv.command) + ")";
        require(tv.command == 3 * i + 2, label + " is not on the script's schedule");
        // The row against the feed it came from: the tape's prices are the
        // next two opens, and its lot is the floor of the exact quantity --
        // one step under it on the single row named above.
        require(std::abs(tv.entry_price - kFeed[tv.command + 1].open) < 0.005
                    && std::abs(tv.exit_price - kFeed[tv.command + 2].open) < 0.005,
                label + " tape price is not the feed's open");
        const long long cents = window_cents(tv.command);
        const bool whole_lot = cents % 3 == 0;
        whole_lot_rows += whole_lot;
        require(cents / 3 - tv.lots == (whole_lot && i + 1 == kStepUnderTrade ? 1 : 0),
                label + " tape lot does not match the feed");

        const Trade trade = host.get_report_trade(i);
        require_schedule(trade, tv.command, label);
        const long long lots = filled_lots(trade, label);
        require(lots == tv.lots, label + " filled " + std::to_string(lots)
                + " lot steps, TradingView filled " + std::to_string(tv.lots));
    }
    require(whole_lot_rows == 5, "the 18 rows hold five whole-lot exact quantities");
}

void offset_controls() {
    for (int sign : {-1, 1}) {
        const std::string form = sign < 0 ? "offset -0.25" : "offset +0.25";
        SmaQuantityHost host(0.25 * sign);
        run_feed(host, form);
        for (int i = 0; i < kTapeTrades; ++i) {
            const int command = 3 * i + 2;
            const std::string label = form + " trade " + std::to_string(i + 1);
            // The exact quantity in twelfths of a lot step: cents / 3 +- 1/4.
            const long long twelfths = 4 * window_cents(command) + 3 * sign;
            require(twelfths % 12 != 0, label + " exact quantity is on a lot boundary");
            const Trade trade = host.get_report_trade(i);
            require_schedule(trade, command, label);
            const long long lots = filled_lots(trade, label);
            require(lots == twelfths / 12, label + " filled " + std::to_string(lots)
                    + " lot steps, exact floor " + std::to_string(twelfths / 12));
        }
    }
}
}  // namespace

int main() {
    try {
        tradingview_rows();
        offset_controls();
        std::cout << "SMA quantity tape: " << kTapeTrades << " TradingView rows and "
                  << 2 * kTapeTrades << " offset control rows matched\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
