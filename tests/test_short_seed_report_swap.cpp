// R5 wave H, lane H-MEASURE, row G2-21: the short-seed report swaps, measured
// against the kernel's own lot identities.
//
// Finding 272's short-seed collision (tests/oracle/test_oracle_short_seed_percent.cpp):
// a SHORT seed S held, then in one bar entry(Long); entry(Short);
// close(Long); close(Short) under percent-of-equity (or cash) default
// sizing. The adapter submits the three live objects so that generic matching
// executes the "__close__Short" artifact BEFORE the final Short
// (PineExecutionAdapter::flush_pending_same_bar_commands arms the
// ShortSeedPlan); their acceptance handles are therefore in the opposite
// order to ab9714be's source report incarnations. The kernel books every row
// with the incarnation of the lot it closed (build_close_trade_with_costs,
// the shared producer); PineStrategyHost::project_short_seed_report_rows then
// swaps two rows' entry_incarnation -- every row still carrying the
// artifact's handle takes the final Short's, and the rows the closing event
// booked under the final Short's handle take the artifact's -- once the
// remnant is closed by a close of the seed id, and only for a non-FIXED
// default (report_swap_pending, pine_adapter.cpp). Nothing else of any row
// changes: the v19 closed-row digest folds no incarnation, and the H-MEASURE
// mutant that skips the swap (instrumented scratch build) fails exactly the
// two incarnation checks of test_native_oracle_short_seed_percent_full_l2.
//
// This row runs the oracle's remnant case once and reads both sides of the
// same run: the kernel's identities (AcceptedEvent handles, each
// ExecutionAppliedEvent's opened lot, native_open_lots before the remnant is
// closed) and the adapter's projected rows. A second case closes nothing
// after the collision: the swap stays pending and the rows keep the
// kernel's identities. TradingView exports no incarnation, so no tape can
// measure this projection; it is a report identity for the C ABI
// (strategy_closed_trade_entry_incarnation) and the corpus CSV's "Engine
// entry incarnation" column, where the grader's distinct-entry gate reads
// only whether two same-key entries differ -- true before and after the swap.
//
// Section 3 replays the `lab tv` tape hm-g221-short-seed (ws-report-v1,
// rangeProof covered; fixtures/g221_short_seed) -- the same collision once a
// day on BINANCE:ETHUSDT.P 15m, 2025-06-02..05, on the corpus feed: the Pine
// host books TradingView's 13 rows (three collisions that end flat, one that
// leaves a remnant) row for row, and the one day whose remnant a close of the
// seed id closes is the one day the swap runs.
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#ifndef PINEFORGE_G221_FIXTURE_DIR
#error "PINEFORGE_G221_FIXTURE_DIR must name tests/fixtures/g221_short_seed"
#endif

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int passed = 0;
int failed = 0;

#define CHECK(x) do { \
    if (x) { ++passed; } \
    else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } \
} while (0)

Bar make_bar(double o, double h, double l, double c, std::int64_t ts) { return {o, h, l, c, 1000.0, ts}; }

class ShortSeed final : public source::PineStrategyHost {
public:
    explicit ShortSeed(bool close_remnant) : close_remnant_(close_remnant) {
        source::PineStrategyConfig c;
        c.initial_capital = 1000000.0;
        c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        c.default_qty_value = 10.0;
        c.pyramiding = 1;
        c.commission_value = 0.0;
        c.slippage = 0;
        configure_pine_strategy(c);
        fixture_retain_all_events();
    }
    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i == 0) strategy_entry("Short", false);
        if (i == 1) {
            strategy_entry("Long", true);
            strategy_entry("Short", false);
            strategy_close("Long");
            strategy_close("Short");
        }
        if (i == 2) {
            lots_before_close = native_open_lots(bar.close);
            if (close_remnant_) strategy_close("Short");
        }
    }
    std::vector<NativeOpenLot> lots_before_close;
private:
    bool close_remnant_;
};

struct Identities {
    std::optional<std::uint64_t> seed, long_entry, final_short, artifact;
    std::vector<no::ExecutionAppliedEvent> applied;
};

Identities read_identities(const ShortSeed& host) {
    Identities out;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* accepted = std::get_if<no::AcceptedEvent>(&*row.command)) {
            const std::string& label = accepted->request().label;
            const std::uint64_t inc = accepted->handle().incarnation;
            if (label == "Short") (out.seed ? out.final_short : out.seed) = inc;
            if (label == "Long") out.long_entry = inc;
            // The first "__close__Short" is the collision's artifact; a later
            // one is the script's close of the remnant.
            if (label == "__close__Short" && !out.artifact) out.artifact = inc;
        } else if (const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&*row.command)) {
            out.applied.push_back(*applied);
        }
    }
    return out;
}

// The lot each applied event opened, by the handle that opened it.
std::optional<std::uint64_t> opened_by(const Identities& ids, std::uint64_t handle) {
    for (const auto& a : ids.applied)
        if (a.handle().incarnation == handle && a.opened_units != 0.0) return a.opened_lot_incarnation;
    return std::nullopt;
}

void test_remnant_closed() {
    std::printf("1. remnant closed by close(\"Short\"): the adapter swaps two rows' identities\n");
    const Bar bars[] = {
        make_bar(100.0, 100.0, 100.0, 100.0, 600'000),
        make_bar(100.0, 100.5, 89.5, 90.0, 1'200'000),
        make_bar(90.0, 90.5, 89.5, 90.0, 1'800'000),
        make_bar(90.0, 90.5, 89.5, 90.0, 2'400'000),
        make_bar(90.0, 90.0, 90.0, 90.0, 3'000'000),
    };
    ShortSeed host(true);
    host.run(bars, 5);
    CHECK(host.last_error().empty());
    const Identities ids = read_identities(host);
    CHECK(ids.seed && ids.long_entry && ids.final_short && ids.artifact);
    CHECK(host.trade_count() == 4);
    if (!(ids.seed && ids.long_entry && ids.final_short && ids.artifact) || host.trade_count() != 4) return;
    const std::uint64_t L = *ids.long_entry, F = *ids.final_short, A = *ids.artifact;
    std::printf("  handles: seed=%llu Long=%llu final Short=%llu __close__Short=%llu\n",
                (unsigned long long)*ids.seed, (unsigned long long)L,
                (unsigned long long)F, (unsigned long long)A);
    // The adapter submits the artifact before the final Short.
    CHECK(A == L + 1);
    CHECK(F == L + 2);
    // The kernel's identities: the artifact's lot and the final Short's lot
    // are each booked under their own handle; before the remnant is closed the
    // kernel book holds the final Short's lot alone.
    CHECK(opened_by(ids, A) == std::optional<std::uint64_t>(A));
    CHECK(opened_by(ids, F) == std::optional<std::uint64_t>(F));
    CHECK(host.lots_before_close.size() == 1);
    if (host.lots_before_close.size() == 1) {
        CHECK(host.lots_before_close[0].entry_incarnation == F);
        CHECK(host.lots_before_close[0].entry_label == "Short");
    }
    const Trade& seed = host.get_trade(0);
    const Trade& zero1 = host.get_trade(1);
    const Trade& zero2 = host.get_trade(2);
    const Trade& remnant = host.get_trade(3);
    for (int i = 0; i < 4; ++i) {
        const Trade& t = host.get_trade(i);
        std::printf("  row %d %s %s -> %s qty=%.6f incarnation=%llu\n", i, t.is_long ? "L" : "S",
                    t.entry_id.c_str(), t.exit_id.c_str(), t.qty, (unsigned long long)t.entry_incarnation);
    }
    CHECK(seed.entry_id == "Short" && seed.entry_incarnation == *ids.seed);
    CHECK(zero1.entry_id == "Long" && zero1.entry_incarnation == L);
    // The adapter's projection: the artifact row reports the final Short's
    // handle, the remnant row the artifact's -- the kernel booked each the
    // other way round (A and F above). ab9714be's numbering: Long, Long+1,
    // Long+2 (the oracle's `remnant == zero1 + 1`, `zero2 == zero1 + 2`).
    CHECK(zero2.entry_id == "__close__Short");
    CHECK(zero2.entry_incarnation == F);
    CHECK(zero2.entry_incarnation != *opened_by(ids, A));
    CHECK(remnant.entry_id == "Short");
    CHECK(remnant.entry_incarnation == A);
    CHECK(remnant.entry_incarnation != host.lots_before_close[0].entry_incarnation);
    CHECK(remnant.entry_incarnation == zero1.entry_incarnation + 1);
    CHECK(zero2.entry_incarnation == zero1.entry_incarnation + 2);
}

void test_remnant_open() {
    std::printf("2. nothing closes the remnant: the swap stays pending, rows keep the kernel's identities\n");
    const Bar bars[] = {
        make_bar(100.0, 100.0, 100.0, 100.0, 600'000),
        make_bar(100.0, 100.5, 89.5, 90.0, 1'200'000),
        make_bar(90.0, 90.5, 89.5, 90.0, 1'800'000),
        make_bar(90.0, 90.5, 89.5, 90.0, 2'400'000),
    };
    ShortSeed host(false);
    host.run(bars, 4);
    CHECK(host.last_error().empty());
    const Identities ids = read_identities(host);
    CHECK(ids.long_entry && ids.final_short && ids.artifact);
    if (!(ids.long_entry && ids.final_short && ids.artifact)) return;
    const std::uint64_t A = *ids.artifact;
    int artifact_rows = 0;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        std::printf("  row %d %s %s -> %s qty=%.6f incarnation=%llu%s\n", i, t.is_long ? "L" : "S",
                    t.entry_id.c_str(), t.exit_id.c_str(), t.qty,
                    (unsigned long long)t.entry_incarnation, t.open_at_end ? " (range end)" : "");
        if (t.entry_id == "__close__Short") {
            ++artifact_rows;
            CHECK(t.entry_incarnation == A);  // the kernel's, unswapped
        }
    }
    CHECK(artifact_rows == 1);
    // The remnant is still open at the range's end: its report row (the
    // shared producer's range-end row) carries the final Short's lot handle.
    // ab9714be's numbering would have given the artifact row Long + 2 and the
    // remnant Long + 1 here as well; the swap never runs, so neither moves.
    bool range_end_remnant = false;
    for (int i = host.trade_count(); i < host.report_trade_count(); ++i) {
        const Trade& t = host.get_report_trade(i);
        std::printf("  report row %d %s %s qty=%.6f incarnation=%llu%s\n", i, t.is_long ? "L" : "S",
                    t.entry_id.c_str(), t.qty, (unsigned long long)t.entry_incarnation,
                    t.open_at_end ? " (range end)" : "");
        if (t.open_at_end && t.entry_id == "Short") {
            range_end_remnant = true;
            CHECK(t.entry_incarnation == *ids.final_short);
        }
    }
    CHECK(range_end_remnant);
}

// ── 3. TradingView's tape ────────────────────────────────────────────────

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close, volume;
};
#include "fixtures/g221_short_seed/bars.inc"

int minute_of_day(std::int64_t ms) { return static_cast<int>((ms / 60000) % 1440); }

// hm-g221-short-seed/strategy.pine as generated code runs it.
class TapeShortSeed final : public source::PineStrategyHost {
public:
    TapeShortSeed() {
        source::PineStrategyConfig c;
        c.initial_capital = 1000000.0;
        c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        c.default_qty_value = 10.0;
        c.pyramiding = 1;
        c.commission_value = 0.0;
        c.slippage = 0;
        configure_pine_strategy(c);
        set_syminfo_mintick(0.01);
        fixture_retain_all_events();
    }
    void on_source_bar(const Bar&) override {
        const int m = minute_of_day(current_bar_.timestamp);
        const double pos = live_position_size();
        if (m == 15 && pos == 0.0) strategy_entry("Short", false);
        if (m == 30 && pos < 0.0) {
            strategy_entry("Long", true);
            strategy_entry("Short", false);
            strategy_close("Long");
            strategy_close("Short");
        }
        if (m == 60 && pos != 0.0) strategy_close("Short");
        if (m == 90 && pos != 0.0) strategy_close_all();
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
    bool is_long = false;
    std::int64_t entry_ms = 0, exit_ms = 0;
    double entry = 0, exit = 0, qty = 0;
    std::string entry_signal, exit_signal;
};

std::vector<TvRow> read_tape() {
    std::ifstream in(std::string(PINEFORGE_G221_FIXTURE_DIR) + "/hm-g221-short-seed/tv_trades.csv");
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
        if (cell[1].rfind("Entry", 0) == 0) { row.entry_ms = ms; row.entry = std::stod(cell[4]); row.entry_signal = cell[3]; }
        else { row.exit_ms = ms; row.exit = std::stod(cell[4]); row.exit_signal = cell[3]; }
    }
    return rows;
}

void test_tape() {
    std::printf("3. TradingView tape hm-g221-short-seed: four collisions, 13 rows\n");
    std::vector<Bar> bars;
    for (const FeedBar& r : kBars) bars.push_back({r.open, r.high, r.low, r.close, r.volume, r.ts});
    TapeShortSeed host;
    host.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    CHECK(host.last_error().empty());
    const std::vector<TvRow> tv = read_tape();
    CHECK(tv.size() == 13);
    CHECK(host.trade_count() == static_cast<int>(tv.size()));
    int swapped = 0;
    for (std::size_t i = 0; i < tv.size() && static_cast<int>(i) < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(static_cast<int>(i));
        std::printf("  #%zu %s %s -> %s qty=%.4f (TV %.4f, %s -> %s) incarnation=%llu\n", i + 1,
                    t.is_long ? "L" : "S", t.entry_id.c_str(), t.exit_id.c_str(), t.qty, tv[i].qty,
                    tv[i].entry_signal.c_str(), tv[i].exit_signal.c_str(),
                    (unsigned long long)t.entry_incarnation);
        CHECK(t.is_long == tv[i].is_long);
        CHECK(t.entry_time == tv[i].entry_ms);
        CHECK(t.exit_time == tv[i].exit_ms);
        CHECK(std::abs(t.entry_price - tv[i].entry) < 1e-6);
        CHECK(std::abs(t.exit_price - tv[i].exit) < 1e-6);
        // TradingView prints the size truncated to 4 decimals (39.5353 for the
        // engine's 39.53538...); the sizing itself is not this row's subject.
        CHECK(std::abs(t.qty - tv[i].qty) < 1e-4);
    }
    // The remnant day (trades 8..10): the artifact row reports a handle one
    // above the remnant's -- ab9714be's numbering -- only because the swap ran.
    if (host.trade_count() == 13) {
        const Trade& zero1 = host.get_trade(7);
        const Trade& zero2 = host.get_trade(8);
        const Trade& remnant = host.get_trade(9);
        CHECK(zero2.entry_id == "__close__Short");
        CHECK(remnant.entry_id == "Short" && !remnant.is_long);
        CHECK(remnant.entry_incarnation == zero1.entry_incarnation + 1);
        CHECK(zero2.entry_incarnation == zero1.entry_incarnation + 2);
        swapped = zero2.entry_incarnation > remnant.entry_incarnation;
        // The three flat days keep the kernel's numbering: artifact = Long + 1.
        for (int first : {1, 4, 11}) {
            const Trade& l = host.get_trade(first);
            const Trade& a = host.get_trade(first + 1);
            CHECK(l.entry_id == "Long" && a.entry_id == "__close__Short");
            CHECK(a.entry_incarnation == l.entry_incarnation + 1);
        }
    }
    CHECK(swapped == 1);
}

}  // namespace

int main() {
    test_remnant_closed();
    test_remnant_open();
    test_tape();
    std::printf("test_short_seed_report_swap: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
