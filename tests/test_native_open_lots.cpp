// R5 gap lane N18: a source-free host observes its open lots through the
// public owning snapshot NativeStrategyHost::native_open_lots(mark) — the
// strategy.opentrades.* surface of a bare host. Every row is what the
// physical book already holds: the lot's identity (ordinal, entry
// incarnation, position cycle), its booking facts (entry label / comment /
// time / bar / price, signed units), the entry fee still on the lot, the
// live P&L at the mark and the excursion the kernel has sampled so far.
//
// The scenario pyramids a long three times, reduces it partially (FIFO: one
// lot closes, the next splits), reverses into a short and flattens. At every
// calculation the rows are cross-checked three ways: against the protected
// strategy.opentrades.* accessors at the same point, against
// native_marked_equity(mark) (the balance plus the rows' P&L is the marked
// equity), and — once each lot has closed — against the closed row it
// produced. Reading the snapshot moves nothing: a twin host that never reads
// it ends with the same continuation hash, broker hash, rows and events.
//
// Nothing here reaches a source or compat header: this TU builds and runs in
// the kernel-only profile.
#include "native_current_fixture.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace r4_test;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool bits_eq(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// Flat bars: every matching point's price is the bar's own price, so the
// excursion a lot can have sampled is exactly the run of closes since its
// entry, which is the independent oracle the expectations below are walked
// from. AfterCalculation close execution: a market request submitted in the
// calculation of bar k fills at bar k's close, after the callback returns.
//
//   idx:    0    1    2    3    4    5    6    7    8   9   10
const std::vector<double> kPrices = {100, 100, 102, 101, 104, 106, 103, 101, 99, 100, 100};
//
//   1: Transact +1 "L1"/"first"   -> lot @100
//   2: Transact +2 "L2"/"second"  -> lot @102
//   4: Transact +1 "L3"/"third"   -> lot @104
//   6: Reduce 1.5 "partial"       -> FIFO: all of L1 and half of L2 close @103
//   7: ReverseTo -1 "REV"/"flip"  -> the rest closes @101, a short lot opens @101
//   9: Flatten "flat"             -> the short closes @100
constexpr double kFee = 2.0;   // CashPerExecution: one ticket per execution

no::Request labelled(no::Request request, const char* label, const char* comment) {
    request.label = label;
    request.comment = comment;
    return request;
}

void rule(Host& h) {
    switch (h.calculations - 1) {
    case 1: put(h, labelled(tx(1.0), "L1", "first")); break;
    case 2: put(h, labelled(tx(2.0), "L2", "second")); break;
    case 4: put(h, labelled(tx(1.0), "L3", "third")); break;
    case 6: put(h, labelled(reduce(1.5), "partial", "")); break;
    case 7: put(h, labelled({no::ReverseTo{-1.0}, "", ""}, "REV", "flip")); break;
    case 9: put(h, labelled(flat(), "flat", "")); break;
    default: break;
    }
}

// The fixture host plus the protected strategy.opentrades.* accessors the
// snapshot is compared against, read at the same calculation point.
struct LotHost : Host {
    std::map<int, std::vector<NativeOpenLot>> snaps;   // by calculation index
    std::map<int, double> marked;                       // native_marked_equity at the mark
    std::map<int, std::size_t> closed_so_far;           // closed rows at that calculation

    double p_profit(int i) const { return open_trade_profit(i); }
    double p_commission(int i) const { return open_trade_commission(i); }
    double p_entry_price(int i) const { return open_trade_entry_price(i); }
    int64_t p_entry_time(int i) const { return open_trade_entry_time(i); }
    int p_entry_bar(int i) const { return open_trade_entry_bar_index(i); }
    std::string p_entry_id(int i) const { return open_trade_entry_id(i); }
    std::string p_entry_comment(int i) const { return open_trade_entry_comment(i); }
    double p_size(int i) const { return open_trade_size(i); }
    double p_runup(int i) const { return open_trade_max_runup(i); }
    double p_drawdown(int i) const { return open_trade_max_drawdown(i); }
    std::string p_direction(int i) const { return open_trade_direction(i); }
    uint64_t broker_hash() const { return broker_state_hash(); }
};

void observe(LotHost& h) {
    const int idx = h.calculations - 1;
    const double mark = kPrices[static_cast<std::size_t>(idx)];
    const auto rows = h.native_open_lots(mark);
    h.snaps[idx] = rows;
    h.marked[idx] = h.native_marked_equity(mark);
    h.closed_so_far[idx] = h.rows().size();

    // The snapshot is the book: one row per lot, in book order, and the
    // signed units sum to the physical position.
    const auto position = h.physical_position();
    CHECK(rows.size() == position.lot_count);
    CHECK(rows.size() == h.lots().size());
    double units = 0.0;
    double pnl = 0.0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const NativeOpenLot& row = rows[i];
        CHECK(row.ordinal == i);
        CHECK(row.cycle == h.cycle());
        CHECK(bits_eq(row.mark, mark));
        units += row.signed_units;
        pnl += row.unrealized_pnl;
        // The protected accessors read the same lot at the same point: the
        // bar's close is the mark here and it has already been sampled, so
        // the excursion agrees exactly and so does the fee-net P&L.
        const int k = static_cast<int>(i);
        CHECK(bits_eq(row.unrealized_pnl, h.p_profit(k)));
        CHECK(bits_eq(row.entry_commission, h.p_commission(k)));
        CHECK(bits_eq(row.entry_price, h.p_entry_price(k)));
        CHECK(row.entry_time_ms == h.p_entry_time(k));
        CHECK(row.entry_bar_index == h.p_entry_bar(k));
        CHECK(row.entry_label == h.p_entry_id(k));
        CHECK(row.entry_comment == h.p_entry_comment(k));
        CHECK(bits_eq(std::abs(row.signed_units), h.p_size(k)));
        CHECK(bits_eq(row.favorable_excursion, h.p_runup(k)));
        CHECK(bits_eq(row.adverse_excursion, h.p_drawdown(k)));
        CHECK((row.side == no::Side::Long) == (h.p_direction(k) == "long"));
        CHECK((row.signed_units > 0.0) == (row.side == no::Side::Long));
        CHECK(row.entry_incarnation == h.lots()[i].entry_incarnation);
        CHECK(row.entry_incarnation != 0);
        CHECK(row.favorable_excursion >= 0.0 && row.adverse_excursion >= 0.0);
    }
    CHECK(bits_eq(units, position.signed_units));
    // Marked equity is the realized balance plus the rows' fee-net P&L, in
    // the kernel's own summation order.
    if (!rows.empty()) {
        double equity = h.balance();
        for (const auto& row : rows) equity += row.unrealized_pnl;
        CHECK(bits_eq(equity, h.marked[idx]));
    } else {
        CHECK(bits_eq(h.balance(), h.marked[idx]));
    }

    // A NaN mark keeps every booking fact and folds nothing: the three
    // marked fields are NaN and the excursion is the sampled one alone,
    // which at this point (the close already sampled) is the same value.
    const auto unmarked = h.native_open_lots(kNaN);
    CHECK(unmarked.size() == rows.size());
    for (std::size_t i = 0; i < unmarked.size() && i < rows.size(); ++i) {
        CHECK(std::isnan(unmarked[i].mark));
        CHECK(std::isnan(unmarked[i].unrealized_pnl));
        CHECK(unmarked[i].ordinal == rows[i].ordinal);
        CHECK(unmarked[i].entry_incarnation == rows[i].entry_incarnation);
        CHECK(unmarked[i].entry_label == rows[i].entry_label);
        CHECK(bits_eq(unmarked[i].entry_price, rows[i].entry_price));
        CHECK(bits_eq(unmarked[i].signed_units, rows[i].signed_units));
        CHECK(bits_eq(unmarked[i].entry_commission, rows[i].entry_commission));
        CHECK(bits_eq(unmarked[i].favorable_excursion, rows[i].favorable_excursion));
        CHECK(bits_eq(unmarked[i].adverse_excursion, rows[i].adverse_excursion));
    }
}

const NativeOpenLot* find_lot(const std::vector<NativeOpenLot>& rows, const char* label) {
    for (const auto& row : rows) if (row.entry_label == label) return &row;
    return nullptr;
}

// One closed row against the snapshot of its lot taken at the calculation
// whose close it exited at (the mark is that close, so the fold is the
// fill). `closed` is the row's quantity, `share` = closed / lot units.
void row_from_lot(const Trade& row, const NativeOpenLot& lot, double closed,
                  double* exit_fee_seen) {
    const double share = closed / std::abs(lot.signed_units);
    CHECK(row.is_long == (lot.side == no::Side::Long));
    CHECK(bits_eq(row.qty, closed));
    CHECK(bits_eq(row.entry_price, lot.entry_price));
    CHECK(row.entry_time == lot.entry_time_ms);
    CHECK(row.entry_bar_index == lot.entry_bar_index);
    CHECK(row.entry_id == lot.entry_label);
    CHECK(row.entry_comment == lot.entry_comment);
    CHECK(row.entry_incarnation == lot.entry_incarnation);
    CHECK(bits_eq(row.exit_price, lot.mark));
    // Gross move of the closed slice == the lot's fee-net P&L at the mark plus
    // its entry fee, scaled to the slice; the row's own pnl is that gross
    // less the row's commission (entry share + exit ticket).
    const double gross_lot = lot.unrealized_pnl + lot.entry_commission;
    near(row.pnl + row.commission, gross_lot * share);
    const double entry_share = lot.entry_commission * share;
    CHECK(row.commission >= entry_share - 1e-12);
    *exit_fee_seen += row.commission - entry_share;
    // The closed row reports excursions on TradingView's net-open-profit
    // basis (kernel row builder): favorable less the entry share, floored at
    // zero; adverse plus the entry share. The snapshot reports the gross
    // price excursion in account currency, mark folded in.
    near(row.max_runup, std::max(0.0, lot.favorable_excursion * share - entry_share));
    near(row.max_drawdown, std::max(0.0, lot.adverse_excursion * share + entry_share));
}

void expect_lot(const NativeOpenLot& row, no::Side side, const char* label,
                const char* comment, int entry_idx, double entry_price,
                double signed_units, double fee, double pnl, double fav, double adv) {
    CHECK(row.side == side);
    CHECK(row.entry_label == label);
    CHECK(row.entry_comment == comment);
    CHECK(row.entry_bar_index == entry_idx);
    // The AfterCalculation close point is timed at the bar's close: one
    // interval after the open the feed stamps the bar with.
    CHECK(row.entry_time_ms == T + static_cast<int64_t>(entry_idx + 1) * 60000);
    near(row.entry_price, entry_price);
    near(row.signed_units, signed_units);
    near(row.entry_commission, fee);
    near(row.unrealized_pnl, pnl);
    near(row.favorable_excursion, fav);
    near(row.adverse_excursion, adv);
}

void pyramided_partial_reversal() {
    LotHost h;
    h.calculation = [](Host& host) {
        auto& lots = static_cast<LotHost&>(host);
        observe(lots);
        rule(lots);
    };
    run(h, spec("n18-open-lots", kFee), {100, 100, 102, 101, 104, 106, 103, 101, 99, 100, 100});
    completed(h);
    REQUIRE(h.snaps.size() == kPrices.size());

    // idx 6, mark 103: three long lots. Samples since entry, ×qty:
    //   L1 @100 ×1: 100 102 101 104 106 103 -> fav 6, adv 0; pnl 3 - 2
    //   L2 @102 ×2: 102 101 104 106 103     -> fav 8, adv 2; pnl 2 - 2
    //   L3 @104 ×1: 104 106 103             -> fav 2, adv 1; pnl -1 - 2
    {
        const auto& rows = h.snaps.at(6);
        REQUIRE(rows.size() == 3);
        expect_lot(rows[0], no::Side::Long, "L1", "first", 1, 100.0, 1.0, kFee, 1.0, 6.0, 0.0);
        expect_lot(rows[1], no::Side::Long, "L2", "second", 2, 102.0, 2.0, kFee, 0.0, 8.0, 2.0);
        expect_lot(rows[2], no::Side::Long, "L3", "third", 4, 104.0, 1.0, kFee, -3.0, 2.0, 1.0);
        CHECK(rows[0].entry_incarnation < rows[1].entry_incarnation);
        CHECK(rows[1].entry_incarnation < rows[2].entry_incarnation);
        near(h.marked.at(6), 10000.0 + 1.0 + 0.0 - 3.0);
    }
    // idx 7, mark 101: the partial closed L1 and half of L2 at 103. The
    // survivor keeps L2's identity, 1.5 units, three quarters of the fee and
    // of the carried excursion; then 101 is sampled: (101-102)×1.5 = -1.5.
    //   L2' @102 ×1.5: carried fav 6, adv 1.5; sample 101 -> fav 6, adv 1.5; pnl -1.5 - 1.5
    //   L3  @104 ×1  : 104 106 103 101 -> fav 2, adv 3; pnl -3 - 2
    {
        const auto& rows = h.snaps.at(7);
        REQUIRE(rows.size() == 2);
        expect_lot(rows[0], no::Side::Long, "L2", "second", 2, 102.0, 1.5, 1.5, -3.0, 6.0, 1.5);
        expect_lot(rows[1], no::Side::Long, "L3", "third", 4, 104.0, 1.0, kFee, -5.0, 2.0, 3.0);
        CHECK(rows[0].entry_incarnation == h.snaps.at(6)[1].entry_incarnation);
        CHECK(rows[1].entry_incarnation == h.snaps.at(6)[2].entry_incarnation);
        CHECK(rows[0].cycle == h.snaps.at(6)[0].cycle);
        // Two closed rows so far: L1 (1.0) and the L2 slice (0.5), both at 103.
        CHECK(h.closed_so_far.at(7) == 2);
        CHECK(h.closed_so_far.at(6) == 0);
    }
    // idx 8, mark 99: the reversal closed L2' and L3 at 101 and opened a
    // short lot at 101 in a NEW position cycle. The reversal is ONE execution
    // of 3.5 units (2.5 closed, 1 opened), and its one cash ticket is split by
    // units over every slice: the short lot carries 2 × (1 / 3.5) of it.
    //   REV @101 ×-1: 101 99 -> fav 2, adv 0; pnl 2 - 4/7
    const double kRevFee = kFee * (1.0 / 3.5);
    {
        const auto& rows = h.snaps.at(8);
        REQUIRE(rows.size() == 1);
        expect_lot(rows[0], no::Side::Short, "REV", "flip", 7, 101.0, -1.0, kRevFee,
                   2.0 - kRevFee, 2.0, 0.0);
        CHECK(rows[0].cycle == h.snaps.at(7)[0].cycle + 1);
        CHECK(rows[0].entry_incarnation > h.snaps.at(7)[1].entry_incarnation);
        CHECK(h.closed_so_far.at(8) == 4);
        CHECK(h.closed_so_far.at(10) == 5);
    }
    // idx 9, mark 100: the short, marked one point in its favour.
    {
        const auto& rows = h.snaps.at(9);
        REQUIRE(rows.size() == 1);
        expect_lot(rows[0], no::Side::Short, "REV", "flip", 7, 101.0, -1.0, kRevFee,
                   1.0 - kRevFee, 2.0, 0.0);
    }
    // idx 10: flat. The book is empty and so is the snapshot, inside the run
    // and after it.
    CHECK(h.snaps.at(10).empty());
    CHECK(h.native_open_lots(100.0).empty());
    CHECK(h.physical_position().lot_count == 0);
    REQUIRE(h.rows().size() == 5);

    // Every closed row against the snapshot of its lot at the calculation it
    // exited in: the partial's two rows (idx 6), the reversal's two (idx 7),
    // the flatten's one (idx 9). Each execution paid exactly one cash ticket,
    // split by units over every slice it touched: the partial's and the
    // flatten's over their closed rows, the reversal's over its two closed
    // rows AND the short lot it opened.
    {
        double exit_fee = 0.0;
        row_from_lot(h.rows()[0], *find_lot(h.snaps.at(6), "L1"), 1.0, &exit_fee);
        row_from_lot(h.rows()[1], *find_lot(h.snaps.at(6), "L2"), 0.5, &exit_fee);
        near(exit_fee, kFee);
        exit_fee = 0.0;
        row_from_lot(h.rows()[2], *find_lot(h.snaps.at(7), "L2"), 1.5, &exit_fee);
        row_from_lot(h.rows()[3], *find_lot(h.snaps.at(7), "L3"), 1.0, &exit_fee);
        near(exit_fee + h.snaps.at(8)[0].entry_commission, kFee);
        exit_fee = 0.0;
        row_from_lot(h.rows()[4], *find_lot(h.snaps.at(9), "REV"), 1.0, &exit_fee);
        near(exit_fee, kFee);
        // The reversal's close and its opening are one execution: the rows it
        // closed and the lot it opened share the exit / entry point.
        CHECK(h.rows()[3].exit_time == h.snaps.at(8)[0].entry_time_ms);
        CHECK(h.rows()[3].exit_bar_index == h.snaps.at(8)[0].entry_bar_index);
    }
}

// Reading the snapshot is an observation: a host that never reads it runs
// the same book to the same hashes, rows and events.
void reading_moves_nothing() {
    LotHost reader;
    reader.calculation = [](Host& host) {
        auto& lots = static_cast<LotHost&>(host);
        // Read at several marks, including outside the bar's range and NaN.
        (void)lots.native_open_lots(kPrices[static_cast<std::size_t>(lots.calculations - 1)]);
        (void)lots.native_open_lots(0.5);
        (void)lots.native_open_lots(1e6);
        (void)lots.native_open_lots(kNaN);
        rule(lots);
    };
    LotHost silent;
    silent.calculation = [](Host& host) { rule(host); };
    const std::vector<double> prices(kPrices.begin(), kPrices.end());
    run(reader, spec("n18-open-lots-twin", kFee), {100, 100, 102, 101, 104, 106, 103, 101, 99, 100, 100});
    run(silent, spec("n18-open-lots-twin", kFee), {100, 100, 102, 101, 104, 106, 103, 101, 99, 100, 100});
    completed(reader);
    completed(silent);
    CHECK(reader.native_continuation_hash() == silent.native_continuation_hash());
    CHECK(reader.broker_hash() == silent.broker_hash());
    CHECK(reader.native_events(0).size() == silent.native_events(0).size());
    REQUIRE(reader.rows().size() == silent.rows().size());
    for (std::size_t i = 0; i < reader.rows().size(); ++i) {
        const Trade& a = reader.rows()[i];
        const Trade& b = silent.rows()[i];
        CHECK(bits_eq(a.pnl, b.pnl));
        CHECK(bits_eq(a.commission, b.commission));
        CHECK(bits_eq(a.max_runup, b.max_runup));
        CHECK(bits_eq(a.max_drawdown, b.max_drawdown));
        CHECK(a.entry_incarnation == b.entry_incarnation);
        CHECK(a.exit_time == b.exit_time);
    }
    (void)prices;
}

// A host that never configured or ran has no book and no rows.
void empty_before_any_run() {
    LotHost h;
    CHECK(h.native_open_lots(100.0).empty());
    CHECK(h.native_open_lots(kNaN).empty());
}

}  // namespace

int main() {
    test("pyramided-partial-reversal", pyramided_partial_reversal);
    test("reading-moves-nothing", reading_moves_nothing);
    test("empty-before-any-run", empty_before_any_run);
    std::printf("test_native_open_lots: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
