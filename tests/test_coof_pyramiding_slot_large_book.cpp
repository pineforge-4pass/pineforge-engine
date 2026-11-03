// The pyramiding slot count stays exact over a large book of pending limits.
//
// The small book is the next-open settled-slot reversal of
// test_coof_close_entry_pyramiding_slot. The large book adds 150 short
// sell-limit entries of distinct ids that never fill. Each of them is a
// same-side pure limit when the probe below is judged, so the slot count
// meets 150 handles that reach its settled-origin check before it meets the
// settled short. No public work counter covers the host's open-lot visit:
// this row checks the admitted and refused results at that size. It does
// not measure the cost of the visit.
//
// Feed: 40 hourly bars, bar i has O = 100 + 2i, H = O + 3, L = O - 2,
// C = O + 1. The highest high is H39 = 178 + 3 = 181.
// Settings: fixed quantity 1, zero commission and slippage, next-open
// processing, calc_on_order_fills on, and the ANY close rule, so the slot
// count decides alone (no FIFO ledger record count replaces it).
// Pyramiding P is 1, 2 or 200.
//
// Script, in ordinary calculations:
//   bar 5, flat: short limit entries rest0 .. rest149, quantity 1, at 1000.
//     The book is flat, so no cap applies and all 150 work. A sell limit
//     fills only once the price reaches it, and 1000 > 181: none fills.
//   bar 10: long market entry "initial", quantity 1. It fills at O11 = 122.
//   bar 20: close "initial" and short market entry "short", quantity 2.
//     Both execute at O21 = 142. Limit companion: a full close and a short
//     sell limit at C20 - 1 = 140, executable at O21 = 142 and filled there.
//   bar 30: close all, at O31 = 162.
// The close's fill callback on bar 21 sees the short settled (position -2,
// one lot) while the short's own applied notification is still queued.
// That callback submits the probe: a short sell limit of 3 at
// O[k] + 2 = 102 + 2k, with id "short" (the same id) or "additional".
//
// Slots occupied when the probe is judged:
//   short lots in cohort opened lists: none, as the short's notification
//     is still queued;
//   each rest: a pure limit of another id that never filled, so no cohort
//     lists it as opened and no open lot has its origin. 0 each, 0 in all;
//   the short: as a pending market entry of another id it counts 1. For the
//     same id, or as a pure limit, its open lot (origin its incarnation,
//     2 units) is in the book, so it is settled and counts 1.
// The total is 1 with or without the rests. The probe is admitted iff
// 1 < P: refused at P = 1, admitted at P = 2 and at P = 200, as the small
// book is refused at 1 and admitted at 2.
//
// Trades (point value and FX 1, zero costs):
//   initial, long 1: bar 11 at 122 to bar 21 at 142, PnL (142 - 122) * 1 = 20;
//   short, short 2: bar 21 at 142 to bar 31 at 162, PnL (142 - 162) * 2 = -40;
//   P >= 2 only, the probe, short 3, to bar 31 at 162:
//     k = 24, limit 150: H23 = 149 < 150 <= H24 = 151 and O24 = 148 < 150,
//       so it fills on bar 24 at 150, PnL (150 - 162) * 3 = -36;
//     k = 26, limit 154: H25 = 153 < 154 <= H26 = 155 and O26 = 152 < 154,
//       so it fills on bar 26 at 154, PnL (154 - 162) * 3 = -24.
// Bar 25: position -2 with 1 lot, or -(2 + 3) = -5 with 2 lots once a k = 24
// probe has filled. Maximum short lots 1 at P = 1, else 2. Final position 0.
// The 150 rests still work at the probe and on bar 25, and none trades.
// Input mutant: k = 26 moves the probe trade by two bars and its entry
// price by 2 * (26 - 24) = 4, and leaves bar 25 at -2 with 1 lot.
//
// Each case also runs the small book (no rests) and requires the same trade
// list, field for field.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <variant>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

namespace {

using namespace pineforge;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1'704'067'200'000LL;
constexpr std::int64_t kHour = 3'600'000;
constexpr int kRests = 150;
constexpr double kRestLimit = 1000.0;
constexpr double kMintick = 0.01;               // the probe's syminfo mintick
constexpr double kRestLevelTolerance = 1e-9;    // the threshold is grid +- 0.5 tick: exact to rounding
int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);      \
        }                                                                      \
    } while (false)

std::vector<Bar> rising_feed() {
    std::vector<Bar> bars;
    for (int i = 0; i < 40; ++i) {
        const double open = 100.0 + 2.0 * i;
        bars.push_back({open, open + 3.0, open - 2.0, open + 1.0, 1000.0,
                        kStart + i * kHour});
    }
    return bars;
}

class BookProbe final : public source::PineStrategyHost {
public:
    BookProbe(int cap, int rests, bool first_limit, bool same_id, double probe_limit)
        : rests_(rests), first_limit_(first_limit), same_id_(same_id),
          probe_limit_(probe_limit) {
        source::PineStrategyConfig cfg{};
        cfg.initial_capital = 1'000'000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.pyramiding = cap;
        cfg.calc_on_order_fills = true;
        cfg.process_orders_on_close = false;
        cfg.close_entries_rule_any = true;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        configure_pine_strategy(cfg);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(kMintick);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", 0.001);
    }

    bool ordinary_calculation() const {
        const auto point = current_execution_point();
        return point
            && point->decision.coordinate.provenance == NativePriceProvenance::Calculation;
    }

    // Working requests labelled rest<k> that are sell limits at the rest level.
    // The adapter hands the core a limit trigger at its trigger threshold, and
    // for a sell limit that threshold sits half a tick below the grid level the
    // call gave (source_trigger_threshold in src/source/pine_adapter.cpp: the
    // grid plus (upward ? -0.5 : 0.5) times the tick, upward for a sell limit).
    // Measured: 999.995 for a call at 1000 with a mintick of 0.01. The rest
    // level is therefore matched at that threshold, not at the call's level.
    // Identity is the label and the limit kind; the probe places only sells.
    int working_rests() const {
        int count = 0;
        for (const auto& row : native_working_requests()) {
            if (!row.definition) continue;
            const auto& request = row.definition->request;
            const auto* level = std::get_if<native_order::Limit>(&request.trigger);
            if (level && std::fabs(level->price - (kRestLimit - 0.5 * kMintick)) < kRestLevelTolerance
                && request.label.rfind("rest", 0) == 0)
                ++count;
        }
        return count;
    }

    void observe() {
        if (position_side_ == PositionSide::SHORT)
            max_short_lots = std::max(max_short_lots, pyramid_entries_.size());
        if (!saw_bar25 && pine_bar_index() == 25 && ordinary_calculation()) {
            saw_bar25 = true;
            bar25_size = signed_position_size();
            bar25_lots = pyramid_entries_.size();
            bar25_rests = working_rests();
        }
    }

    void on_source_bar(const Bar& bar) override {
        observe();
        const int i = pine_bar_index();
        if (ordinary_calculation()) {
            if (i == 5) {
                for (int k = 0; k < rests_; ++k)
                    strategy_entry("rest" + std::to_string(k), false, kRestLimit, kNaN, 1.0);
            }
            if (i == 10)
                strategy_entry("initial", true, kNaN, kNaN, 1.0);
            if (i == 20) {
                strategy_close(first_limit_ ? "" : "initial", "", kNaN, kNaN, false, 1);
                strategy_entry("short", false, first_limit_ ? bar.close - 1.0 : kNaN,
                               kNaN, 2.0);
            }
            if (i == 30)
                strategy_close("", "", kNaN, kNaN, false);
        } else if (!submitted && i == 21 && signed_position_size() == -2.0) {
            submitted = true;
            submission_bar = i;
            submission_size = signed_position_size();
            submission_lots = pyramid_entries_.size();
            submission_rests = working_rests();
            strategy_entry(same_id_ ? "short" : "additional", false, probe_limit_, kNaN, 3.0);
        }
        observe();
    }

    double final_size() const { return signed_position_size(); }
    bool submitted = false;
    int submission_bar = -1;
    double submission_size = kNaN;
    std::size_t submission_lots = 0;
    int submission_rests = -1;
    bool saw_bar25 = false;
    double bar25_size = kNaN;
    std::size_t bar25_lots = 0;
    int bar25_rests = -1;
    std::size_t max_short_lots = 0;

private:
    int rests_;
    bool first_limit_;
    bool same_id_;
    double probe_limit_;
};

struct TradeRow {
    std::string id;
    bool is_long = false;
    double qty = 0.0;
    int entry_bar = -1;
    int exit_bar = -1;
    double entry_price = 0.0;
    double exit_price = 0.0;
    double pnl = 0.0;
};

bool same_row(const TradeRow& a, const TradeRow& b) {
    return a.id == b.id && a.is_long == b.is_long && a.qty == b.qty
        && a.entry_bar == b.entry_bar && a.exit_bar == b.exit_bar
        && a.entry_price == b.entry_price && a.exit_price == b.exit_price && a.pnl == b.pnl;
}

bool same_rows(const std::vector<TradeRow>& a, const std::vector<TradeRow>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), same_row);
}

void print_rows(const char* label, const std::vector<TradeRow>& rows) {
    std::printf("  %s: %zu trades\n", label, rows.size());
    for (const auto& t : rows) {
        std::printf("    %s %s qty %.17g entry %d @%.17g exit %d @%.17g pnl %.17g\n",
                    t.id.c_str(), t.is_long ? "long" : "short", t.qty, t.entry_bar,
                    t.entry_price, t.exit_bar, t.exit_price, t.pnl);
    }
}

void check_case(int cap, bool first_limit, bool same_id, int limit_bar) {
    std::printf("cap %d, %s first short, probe id %s, probe limit bar %d\n", cap,
                first_limit ? "limit" : "market", same_id ? "short" : "additional", limit_bar);
    const auto bars = rising_feed();
    // The derivation's prices, read back from the feed.
    CHECK(bars[11].open == 122.0);
    CHECK(bars[20].close == 141.0);
    CHECK(bars[21].open == 142.0);
    CHECK(bars[31].open == 162.0);
    CHECK(bars[39].high == 181.0);
    const double probe_limit = bars[limit_bar].open + 2.0;
    CHECK(probe_limit == 102.0 + 2.0 * limit_bar);
    CHECK(bars[limit_bar - 1].high < probe_limit && probe_limit <= bars[limit_bar].high);
    CHECK(bars[limit_bar].open < probe_limit);

    const bool admitted = cap >= 2;
    const bool added_by_25 = admitted && limit_bar <= 25;
    std::vector<TradeRow> expected{
        {"initial", true, 1.0, 11, 21, 122.0, 142.0, 20.0},
        {"short", false, 2.0, 21, 31, 142.0, 162.0, -40.0},
    };
    if (admitted) {
        expected.push_back({same_id ? "short" : "additional", false, 3.0, limit_bar, 31,
                            probe_limit, 162.0, (probe_limit - 162.0) * 3.0});
    }

    std::vector<TradeRow> books[2];
    const int rest_counts[2] = {0, kRests};
    for (int book = 0; book < 2; ++book) {
        const int rests = rest_counts[book];
        BookProbe p(cap, rests, first_limit, same_id, probe_limit);
        p.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
        CHECK(p.last_error().empty());
        if (!p.last_error().empty()) {
            std::printf("  %d rests, run error: %s\n", rests, p.last_error().c_str());
            return;
        }
        for (int i = 0; i < p.trade_count(); ++i) {
            const Trade& t = p.get_trade(i);
            books[book].push_back({t.entry_id, t.is_long, t.qty, t.entry_bar_index,
                                   t.exit_bar_index, t.entry_price, t.exit_price, t.pnl});
            CHECK(t.entry_time == kStart + t.entry_bar_index * kHour);
            CHECK(t.exit_time == kStart + t.exit_bar_index * kHour);
        }
        // The probe was judged in the close's fill callback, before the
        // short's notification, with the whole book working.
        CHECK(p.submitted);
        CHECK(p.submission_bar == 21);
        CHECK(p.submission_size == -2.0);
        CHECK(p.submission_lots == 1U);
        CHECK(p.submission_rests == rests);
        CHECK(same_rows(books[book], expected));
        if (!same_rows(books[book], expected)) {
            std::printf("  %d rests: submission lots %zu, working rests %d; "
                        "bar 25 size %.17g, lots %zu\n",
                        rests, p.submission_lots, p.submission_rests,
                        p.bar25_size, p.bar25_lots);
            print_rows("actual", books[book]);
            print_rows("expected", expected);
        }
        CHECK(p.saw_bar25);
        CHECK(p.bar25_size == (added_by_25 ? -(2.0 + 3.0) : -2.0));
        CHECK(p.bar25_lots == (added_by_25 ? 2U : 1U));
        CHECK(p.bar25_rests == rests);
        CHECK(p.max_short_lots == (admitted ? 2U : 1U));
        CHECK(p.final_size() == 0.0);
    }
    // The 150 rests change no admission: the same trades as the small book.
    CHECK(same_rows(books[1], books[0]));
}

}  // namespace

int main() {
    for (bool first_limit : {false, true}) {
        for (bool same_id : {true, false}) {
            check_case(1, first_limit, same_id, 24);
            check_case(2, first_limit, same_id, 24);
            check_case(200, first_limit, same_id, 24);
            check_case(2, first_limit, same_id, 26);
        }
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
