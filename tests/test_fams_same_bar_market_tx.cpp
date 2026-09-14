/*
 * test_fams_same_bar_market_tx.cpp — round 8 family S: TradingView's same-bar
 * MARKET transaction (strategy.entry(Long) + strategy.entry(Short) +
 * strategy.close(...) on one bar), replayed row for row against the 15 lab
 * tv sensor tapes (ledger note log-20260905t143024z-76025577; tv-tape-famS-*;
 * CME_MINI:ES1! / NQ1! 15m 2025-04-01..15, ws-report-v1, rangeProof covered).
 *
 * Every tape is an 8-bar cycle: k0 seeds a 1-lot position (or nothing, in
 * dbl-flat-full), k2 issues the calls under test, k4 strategy.close_all with
 * comment "pos" + position_size reads the state. The engine runs the same
 * calls on the registry's own bars (test_fams_same_bar_market_tx_data.hpp)
 * and its trade rows must equal TradingView's — same pairing, same fill bar,
 * same price, same signal, same quantity — for every one of the 115 cycles.
 *
 * Rules pinned (PendingOrder::sbmt_member, engine.hpp):
 *   (1) order size frozen at placement (own + opposite position net of an
 *       earlier same-bar close + pending opposite market's open leg);
 *   (2) same-direction over-cap entry dropped unless an opposite market is
 *       pending, then kept and sized by (1), never re-roled at fill;
 *   (3) all BUY market orders fill, then all SELL market orders;
 *   (4) strategy.close(id) sized to the lot it holds at the call; when that
 *       side is gone it fills as a new lot iff its same-id entry is still
 *       pending ("Close entry(s) order X" row), else it is cancelled;
 *   (5) strategy.close(id) with no lot at the call places nothing;
 *   plus the admission census famS-adm-*: the kept over-cap entry is costed
 *   as three lots (held + own + opposite pending) at placement.
 */

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "test_fams_same_bar_market_tx_data.hpp"

using namespace pineforge;
using pineforge::source::PendingOrder;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                           \
        } else {                                                                \
            ++g_pass;                                                           \
        }                                                                       \
    } while (0)

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

enum class Call { EntryLong, EntryShort, EntryShortQ2, CloseLong, CloseShort };

struct Fixture {
    const char* name;
    bool nq;                       // CME_MINI:NQ1! (pv 20) vs ES1! (pv 50)
    double capital;
    double default_qty;
    bool seed_long;                // k0 seed direction
    bool seed;                     // false: dbl-flat-full (no k0 seed)
    std::vector<Call> k2;
    const fams_data::TvRow* rows;
    int nrows;
};

class TapeProbe final : public pineforge::source::PineStrategyHost {
public:
    explicit TapeProbe(const Fixture& f) : f_(f) {
        initial_capital_ = f.capital;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = f.default_qty;
        pyramiding_ = 1;                 // Pine pyramiding=0
        slippage_ = 0;
        commission_value_ = 0.0;
        margin_long_ = 100.0;            // Pine v6 defaults
        margin_short_ = 100.0;
        syminfo_mintick_ = 0.25;
        syminfo_.mintick = 0.25;
        syminfo_.pointvalue = f.nq ? 20.0 : 50.0;
        syminfo_.type = "futures";
        syminfo_.timezone = "America/Chicago";
        syminfo_.session = "1700-1600";
    }

    void on_source_bar(const Bar&) override {
        const int k = bar_index_ % 8;
        if (k == 0 && f_.seed) {
            if (f_.seed_long) strategy_entry("Long", true);
            else strategy_entry("Short", false);
        }
        if (k == 2) {
            for (Call c : f_.k2) {
                switch (c) {
                    case Call::EntryLong: strategy_entry("Long", true); break;
                    case Call::EntryShort: strategy_entry("Short", false); break;
                    case Call::EntryShortQ2:
                        strategy_entry("Short", false, kNaN, kNaN, 2.0); break;
                    case Call::CloseLong: strategy_close("Long"); break;
                    case Call::CloseShort: strategy_close("Short"); break;
                }
            }
        }
        if (k == 4) {
            const double pos = signed_position_size();
            char buf[32];
            std::snprintf(buf, sizeof buf, "pos%d", (int)std::lround(pos));
            strategy_close("", buf);
        }
    }

private:
    Fixture f_;
};

// TradingView's "Signal" cell: the comment when one was given, else the order
// id; a strategy.close market order without a comment prints
// "Close entry(s) order <id>" — the lab verifier's rendering of the engine's
// "__close__<id>" tag (the same convention finding 272 pinned).
std::string signal_of(const std::string& id, const std::string& comment) {
    if (!comment.empty()) return comment;
    const std::string prefix = "__close__";
    if (id.compare(0, prefix.size(), prefix) == 0) {
        return "Close entry(s) order " + id.substr(prefix.size());
    }
    return id;
}

// A TradingView trade = one entry row + one exit row sharing a trade number.
struct Pair {
    bool is_long;
    int64_t entry_ts;
    std::string entry_sig;
    double entry_px;
    int64_t exit_ts;
    std::string exit_sig;
    double exit_px;
    double qty;
    std::string key() const {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%c|%lld|%s|%.4f|%lld|%s|%.4f|%.4f",
                      is_long ? 'L' : 'S', (long long)entry_ts, entry_sig.c_str(),
                      entry_px, (long long)exit_ts, exit_sig.c_str(), exit_px, qty);
        return buf;
    }
};

std::map<std::string, int> tv_pairs(const fams_data::TvRow* rows, int n) {
    std::map<int, Pair> by_n;
    for (int i = 0; i < n; ++i) {
        const fams_data::TvRow& r = rows[i];
        Pair& p = by_n[r.n];
        p.is_long = r.is_long;
        p.qty = r.qty;
        if (r.entry) {
            p.entry_ts = r.ts; p.entry_sig = r.signal; p.entry_px = r.price;
        } else {
            p.exit_ts = r.ts; p.exit_sig = r.signal; p.exit_px = r.price;
        }
    }
    std::map<std::string, int> out;
    for (const auto& kv : by_n) ++out[kv.second.key()];
    return out;
}

std::map<std::string, int> engine_pairs(const TapeProbe& p, int64_t last_tv_ts) {
    std::map<std::string, int> out;
    for (int i = 0; i < p.trade_count(); ++i) {
        const Trade& t = p.get_trade(i);
        if (t.open_at_end || t.exit_time > last_tv_ts) continue;  // past the tape's range
        Pair q;
        q.is_long = t.is_long;
        q.entry_ts = t.entry_time;
        q.entry_sig = signal_of(t.entry_id, t.entry_comment);
        q.entry_px = t.entry_price;
        q.exit_ts = t.exit_time;
        q.exit_sig = signal_of(t.exit_id, t.exit_comment);
        q.exit_px = t.exit_price;
        q.qty = t.qty;
        ++out[q.key()];
    }
    return out;
}

void run_fixture(const Fixture& f) {
    std::printf("tape famS-%s\n", f.name);
    TapeProbe probe(f);
    const fams_data::BarRow* src = f.nq ? fams_data::NQ1_BARS : fams_data::ES1_BARS;
    const int nbars = f.nq ? fams_data::kNQ1Bars : fams_data::kES1Bars;
    std::vector<Bar> bars(nbars);
    for (int i = 0; i < nbars; ++i) {
        bars[i].open = src[i].o; bars[i].high = src[i].h; bars[i].low = src[i].l;
        bars[i].close = src[i].c; bars[i].volume = src[i].v; bars[i].timestamp = src[i].ts;
    }
    probe.run(bars.data(), nbars);

    int64_t last_tv_ts = 0;
    for (int i = 0; i < f.nrows; ++i) last_tv_ts = std::max(last_tv_ts, f.rows[i].ts);
    const auto tv = tv_pairs(f.rows, f.nrows);
    const auto eng = engine_pairs(probe, last_tv_ts);

    int tv_total = 0, eng_total = 0, missing = 0, extra = 0;
    for (const auto& kv : tv) tv_total += kv.second;
    for (const auto& kv : eng) eng_total += kv.second;
    for (const auto& kv : tv) {
        auto it = eng.find(kv.first);
        const int have = it == eng.end() ? 0 : it->second;
        if (have < kv.second) {
            missing += kv.second - have;
            if (missing <= 6) std::fprintf(stderr, "  missing in engine: %s (tv %d, eng %d)\n", kv.first.c_str(), kv.second, have);
        }
    }
    for (const auto& kv : eng) {
        auto it = tv.find(kv.first);
        const int want = it == tv.end() ? 0 : it->second;
        if (kv.second > want) {
            extra += kv.second - want;
            if (extra <= 6) std::fprintf(stderr, "  extra in engine:   %s (tv %d, eng %d)\n", kv.first.c_str(), want, kv.second);
        }
    }
    std::printf("  tv trades %d, engine trades %d, missing %d, extra %d\n",
                tv_total, eng_total, missing, extra);
    CHECK(tv_total == eng_total);
    CHECK(missing == 0);
    CHECK(extra == 0);
}

}  // namespace

int main() {
    using namespace fams_data;
    const std::vector<Call> full = {Call::EntryLong, Call::EntryShort, Call::CloseLong, Call::CloseShort};
    const std::vector<Fixture> fixtures = {
        // name, nq, capital, default qty, seed long, seed, k2 calls, rows
        {"dbl-flat-full", false, 1e8, 1.0, false, false, full, DBL_FLAT_FULL_ROWS, kDBL_FLAT_FULLRows},
        {"dbl-long-full", false, 1e8, 1.0, true, true, full, DBL_LONG_FULL_ROWS, kDBL_LONG_FULLRows},
        {"dbl-long-mirror-closefirst", false, 1e8, 1.0, true, true,
         {Call::EntryShort, Call::EntryLong, Call::CloseShort, Call::CloseLong},
         DBL_LONG_MIRROR_CLOSEFIRST_ROWS, kDBL_LONG_MIRROR_CLOSEFIRSTRows},
        {"dbl-short-closefirst", false, 1e8, 1.0, false, true,
         {Call::CloseShort, Call::CloseLong, Call::EntryLong, Call::EntryShort},
         DBL_SHORT_CLOSEFIRST_ROWS, kDBL_SHORT_CLOSEFIRSTRows},
        {"dbl-short-full", false, 1e8, 1.0, false, true, full, DBL_SHORT_FULL_ROWS, kDBL_SHORT_FULLRows},
        {"dbl-short-noclose", false, 1e8, 1.0, false, true,
         {Call::EntryLong, Call::EntryShort}, DBL_SHORT_NOCLOSE_ROWS, kDBL_SHORT_NOCLOSERows},
        {"dbl-short-onlycloseshort", false, 1e8, 1.0, false, true,
         {Call::EntryLong, Call::EntryShort, Call::CloseShort},
         DBL_SHORT_ONLYCLOSESHORT_ROWS, kDBL_SHORT_ONLYCLOSESHORTRows},
        {"dbl-short-q1-entry2", false, 1e8, 1.0, false, true,
         {Call::EntryLong, Call::EntryShortQ2, Call::CloseLong, Call::CloseShort},
         DBL_SHORT_Q1_ENTRY2_ROWS, kDBL_SHORT_Q1_ENTRY2Rows},
        {"dbl-short-q3", false, 1e8, 3.0, false, true, full, DBL_SHORT_Q3_ROWS, kDBL_SHORT_Q3Rows},
        {"dbl-short-swapped", false, 1e8, 1.0, false, true,
         {Call::EntryShort, Call::EntryLong, Call::CloseLong, Call::CloseShort},
         DBL_SHORT_SWAPPED_ROWS, kDBL_SHORT_SWAPPEDRows},
        {"rev-plus-close", false, 1e8, 1.0, false, true,
         {Call::EntryLong, Call::CloseShort}, REV_PLUS_CLOSE_ROWS, kREV_PLUS_CLOSERows},
        // Admission census: TradingView's default 1,000,000 vs 500,000.
        {"adm-es-1e6", false, 1e6, 1.0, false, true, full, ADM_ES_1E6_ROWS, kADM_ES_1E6Rows},
        {"adm-es-500k", false, 5e5, 1.0, false, true, full, ADM_ES_500K_ROWS, kADM_ES_500KRows},
        {"adm-nq-1e6", true, 1e6, 1.0, false, true, full, ADM_NQ_1E6_ROWS, kADM_NQ_1E6Rows},
        {"adm-nq-500k", true, 5e5, 1.0, false, true, full, ADM_NQ_500K_ROWS, kADM_NQ_500KRows},
    };
    for (const Fixture& f : fixtures) run_fixture(f);
    std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
