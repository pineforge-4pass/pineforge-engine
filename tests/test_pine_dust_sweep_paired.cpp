// R5 wave H, lane H-MEASURE, row G2-32: the Pine source host's 1e-10 lot
// sweep, measured beside the kernel's settlement.
//
// After every applied execution PineStrategyHost::on_native_applied erases
// any lot of at most kQtyEpsilon (1e-10) from the book it shares with the
// kernel (pyramid_entries_, position_qty_, position_entry_price_,
// position_entry_count_), with no closing row and no event, and resets the
// position when nothing is left. The kernel's settlement keeps any positive
// remainder as a lot, and a bare NativeStrategyHost keeps it.
//
// ab9714be's rule (src/engine_orders.cpp settle_position_after_partial_exit,
// :531-541, beside the settlement's `split.kept > 0.0` survivors) reset only a
// WHOLE book of at most kQtyEpsilon; a dust lot beside a live one survived.
// The sweep is therefore not a restatement of it: it also erases a dust lot
// inside a live book (section 2).
//
// What TradingView does with such a remnant (section 3): the `lab tv` tape
// hm-g232-decimal-dust (ws-report-v1, rangeProof covered;
// fixtures/g232_decimal_dust) buys 0.1, 0.2 and 1.0 and sells 0.3 every day.
// TradingView's decimal quantities close the 0.1 and 0.2 lots whole and leave
// the 1.0 lot alone, three rows a day. In binary64 0.1 + 0.2 exceeds 0.3, so
// the kernel's FIFO split leaves a 2.8e-17 remnant of the 0.2 lot beside the
// 1.0 lot; the Pine host sweeps it and books TradingView's twelve rows, the
// bare host keeps it and books it as a row of its own at the flatten.
//
// Sections 1 and 2 are the AUDIT4 probes probe_dust / probe_dust3 (a sell of
// 2 - 1e-11 against two and three 1-unit lots), pinned on both hosts.
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

#ifndef PINEFORGE_G232_FIXTURE_DIR
#error "PINEFORGE_G232_FIXTURE_DIR must name tests/fixtures/g232_decimal_dust"
#endif

namespace {

int passed = 0;
int failed = 0;

#define CHECK(x) do { \
    if (x) { ++passed; } \
    else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } \
} while (0)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kSell = 2.0 - 1e-11;
bool near(double a, double b, double tol = 1e-9) { return std::abs(a - b) <= tol; }

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};
#include "fixtures/g232_decimal_dust/bars.inc"

Bar mk(std::int64_t ts, double o) {
    Bar b;
    b.open = o; b.high = o + 1; b.low = o - 1; b.close = o + 0.5; b.volume = 1; b.timestamp = ts;
    return b;
}

std::vector<Bar> synthetic(int n) {
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) bars.push_back(mk(300000LL * i, 100 + i));
    return bars;
}

source::PineStrategyConfig fixed_config(int pyramiding) {
    source::PineStrategyConfig c;
    c.initial_capital = 100000.0;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = pyramiding;
    c.commission_value = 0.0;
    c.slippage = 0;
    return c;
}

NativeRunSpec bare_spec(const std::string& key, const char* tf) {
    NativeRunSpec spec;
    spec.identity.session_key = key;
    spec.identity.run_number = 1;
    spec.input_tf = tf; spec.script_tf = tf;
    spec.ticker = "ETHUSDT.P"; spec.tickerid = "BINANCE:ETHUSDT.P";
    spec.type = "crypto"; spec.currency = "USDT"; spec.basecurrency = "ETH";
    spec.timezone = "UTC"; spec.session = "24x7";
    spec.initial_capital = 100000.0; spec.point_value = 1.0; spec.account_fx = 1.0;
    spec.price_tick = 0.01; spec.fee_kind = NativeFeeKind::Percent; spec.fee_value = 0.0;
    return spec;
}

// ── 1 and 2: the AUDIT4 shapes ─────────────────────────────────────────────

// `lots` 1-unit entries on bars 0..lots-1, then a sell of 2 - 1e-11.
class PineDust final : public source::PineStrategyHost {
public:
    explicit PineDust(int lots) : lots_(lots) {
        configure_pine_strategy(fixed_config(lots));
        set_syminfo_mintick(0.01);
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i < lots_) strategy_entry("L" + std::to_string(i), true);
        if (i == lots_) strategy_order("S", false, kSell);
    }
    using BacktestEngine::pyramid_entries_;
    using BacktestEngine::position_qty_;
private:
    int lots_;
};

class BareDust final : public NativeStrategyHost {
public:
    explicit BareDust(int lots) : lots_(lots) {}
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (calls_ < lots_) (void)submit_market({no::Transact{1.0}, "L" + std::to_string(calls_), ""});
        if (calls_ == lots_) (void)submit_market({no::Transact{-kSell}, "S", ""});
        ++calls_;
    }
    using BacktestEngine::pyramid_entries_;
    using BacktestEngine::position_qty_;
private:
    int lots_;
    int calls_ = 0;
};

void test_audit_shapes() {
    std::printf("1-2. AUDIT4 probe_dust (2 lots) and probe_dust3 (3 lots): sell 2 - 1e-11\n");
    for (int lots : {2, 3}) {
        const auto bars = synthetic(lots + 4);
        PineDust pine(lots);
        pine.run(bars.data(), static_cast<int>(bars.size()), "5", "5");
        BareDust bare(lots);
        CHECK(bare.configure_native(bare_spec("g232-dust-" + std::to_string(lots), "5")).status
              == NativeSetupStatus::Applied);
        bare.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(pine.last_error().empty());
        CHECK(bare.last_error().empty());
        const auto pine_lots = pine.native_open_lots(bars.back().close);
        const auto bare_lots = bare.native_open_lots(bars.back().close);
        std::printf("  %d lots: pine rows=%d lots=%zu position=%.17g | bare rows=%d lots=%zu position=%.17g\n",
                    lots, pine.trade_count(), pine_lots.size(), pine.position_qty_,
                    bare.trade_count(), bare_lots.size(), bare.position_qty_);
        // Both books charge the sell exactly: rows 1 and 0.99999999999.
        for (const BacktestEngine* host : {static_cast<const BacktestEngine*>(&pine),
                                           static_cast<const BacktestEngine*>(&bare)}) {
            CHECK(host->trade_count() == 2);
            if (host->trade_count() == 2) {
                CHECK(host->get_trade(0).qty == 1.0);
                CHECK(near(host->get_trade(1).qty, 0.99999999999, 1e-15));
            }
        }
        if (lots == 2) {
            // Whole book: the Pine host resets flat -- ab9714be's rule too.
            CHECK(pine_lots.empty());
            CHECK(pine.position_qty_ == 0.0);
            CHECK(bare_lots.size() == 1);
            if (bare_lots.size() == 1) CHECK(near(bare_lots[0].signed_units, 1.000000082740371e-11, 1e-24));
        } else {
            // A dust lot INSIDE a live book: the Pine host erases it; the
            // kernel (and ab9714be's settle rule) keep it.
            CHECK(pine_lots.size() == 1);
            CHECK(pine.position_qty_ == 1.0);
            CHECK(bare_lots.size() == 2);
            CHECK(near(bare.position_qty_, 1.00000000001, 1e-15));
            if (bare_lots.size() == 2) {
                CHECK(near(bare_lots[0].signed_units, 1.000000082740371e-11, 1e-24));
                CHECK(bare_lots[1].signed_units == 1.0);
            }
        }
    }
}

// ── 3: TradingView's decimal quantities ────────────────────────────────────

int minute_of_day(std::int64_t ms) { return static_cast<int>((ms / 60000) % 1440); }

// hm-g232-decimal-dust/strategy.pine, as generated code runs it.
class PineDecimal final : public source::PineStrategyHost {
public:
    PineDecimal() {
        auto c = fixed_config(3);
        c.initial_capital = 1000000.0;
        configure_pine_strategy(c);
        set_syminfo_mintick(0.01);
    }
    void on_source_bar(const Bar&) override {
        const int m = minute_of_day(current_bar_.timestamp);
        const double pos = live_position_size();
        if (m == 15 && pos == 0.0) strategy_entry("A", true, kNaN, kNaN, 0.1);
        if (m == 30 && pos > 0.0) strategy_entry("B", true, kNaN, kNaN, 0.2);
        if (m == 45 && pos > 0.0) strategy_entry("C", true, kNaN, kNaN, 1.0);
        if (m == 60 && pos > 0.0) strategy_order("S", false, 0.3);
        if (m == 120 && pos != 0.0) strategy_close_all();
    }
};

class BareDecimal final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext& ctx) override {
        const int m = minute_of_day(ctx.script_bar_open_ms);
        const double pos = physical_position().signed_units;
        if (m == 15 && pos == 0.0) (void)submit({no::Transact{0.1}, "A", ""});
        if (m == 30 && pos > 0.0) (void)submit({no::Transact{0.2}, "B", ""});
        if (m == 45 && pos > 0.0) (void)submit({no::Transact{1.0}, "C", ""});
        if (m == 60 && pos > 0.0) (void)submit({no::Transact{-0.3}, "S", ""});
        if (m == 120 && pos != 0.0) (void)submit({no::Flatten{}, "flat", ""});
    }
};

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct TvRow {
    bool is_long;
    std::int64_t entry_ms, exit_ms;
    double entry, exit, qty;
};

// tv_trades.csv: one row per side, times UTC+8; trades newest last.
std::vector<TvRow> read_tape() {
    std::ifstream in(std::string(PINEFORGE_G232_FIXTURE_DIR)
                     + "/hm-g232-decimal-dust/tv_trades.csv");
    std::vector<TvRow> rows;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 6) continue;
        const std::size_t n = static_cast<std::size_t>(std::stoi(cell[0]));
        if (rows.size() < n) rows.resize(n);
        int y, mo, d, h, mi;
        if (std::sscanf(cell[2].c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) continue;
        const std::int64_t ms = ((days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 24
                                  + h - 8) * 60 + mi) * 60000LL;
        TvRow& row = rows[n - 1];
        row.is_long = cell[1].find("long") != std::string::npos;
        row.qty = std::stod(cell[5]);
        if (cell[1].rfind("Entry", 0) == 0) { row.entry_ms = ms; row.entry = std::stod(cell[4]); }
        else { row.exit_ms = ms; row.exit = std::stod(cell[4]); }
    }
    return rows;
}

template <std::size_t N>
std::vector<Bar> day_bars(const FeedBar (&rows)[N]) {
    std::vector<Bar> bars;
    for (const FeedBar& r : rows) bars.push_back({r.open, r.high, r.low, r.close, r.volume, r.ts});
    return bars;
}

void test_decimal_tape() {
    std::printf("3. TradingView tape hm-g232-decimal-dust: buy 0.1, 0.2, 1.0; sell 0.3; flatten\n");
    const std::vector<TvRow> tv = read_tape();
    CHECK(tv.size() == 12);
    const std::vector<Bar> days[] = {day_bars(kDay0), day_bars(kDay1), day_bars(kDay2), day_bars(kDay3)};
    std::vector<Trade> pine_rows, bare_rows;
    int bare_dust_rows = 0;
    for (int d = 0; d < 4; ++d) {
        PineDecimal pine;
        pine.run(days[d].data(), static_cast<int>(days[d].size()), "15", "15");
        CHECK(pine.last_error().empty());
        for (int i = 0; i < pine.trade_count(); ++i) pine_rows.push_back(pine.get_trade(i));
        CHECK(pine.native_open_lots(days[d].back().close).empty());

        BareDecimal bare;
        CHECK(bare.configure_native(bare_spec("g232-decimal-" + std::to_string(d), "15")).status
              == NativeSetupStatus::Applied);
        bare.run(days[d].data(), static_cast<int>(days[d].size()));
        CHECK(bare.last_error().empty());
        for (int i = 0; i < bare.trade_count(); ++i) {
            bare_rows.push_back(bare.get_trade(i));
            if (bare.get_trade(i).qty < 1e-10) ++bare_dust_rows;
        }
    }
    std::printf("  TradingView rows=%zu | Pine host rows=%zu | bare host rows=%zu (of which dust rows %d)\n",
                tv.size(), pine_rows.size(), bare_rows.size(), bare_dust_rows);
    for (const Trade& t : bare_rows)
        if (t.qty < 1e-10)
            std::printf("    bare dust row: %s qty=%.17g entry=%.2f exit=%.2f pnl=%.17g\n",
                        t.entry_id.c_str(), t.qty, t.entry_price, t.exit_price, t.pnl);
    // The Pine host books TradingView's rows, trade for trade.
    CHECK(pine_rows.size() == tv.size());
    for (std::size_t i = 0; i < tv.size() && i < pine_rows.size(); ++i) {
        const Trade& p = pine_rows[i];
        CHECK(p.is_long == tv[i].is_long);
        CHECK(p.entry_time == tv[i].entry_ms);
        CHECK(p.exit_time == tv[i].exit_ms);
        CHECK(near(p.entry_price, tv[i].entry, 1e-6));
        CHECK(near(p.exit_price, tv[i].exit, 1e-6));
        CHECK(near(p.qty, tv[i].qty, 1e-9));
    }
    // The bare host keeps the remnant and closes it as a fourth row a day.
    CHECK(bare_rows.size() == 16);
    CHECK(bare_dust_rows == 4);
}

}  // namespace

int main() {
    test_audit_shapes();
    test_decimal_tape();
    std::printf("test_pine_dust_sweep_paired: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
