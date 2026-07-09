// test_security_range_start_na_warmup — pins the opt-in KI-55 HTF warmup flag.
//
// The engine run flag ``security_range_start_na_warmup`` (carried through the
// existing syminfo_metadata channel as an epoch-ms range start) makes every
// ``request.security`` series:
//   (a) start aggregation at the range start (drop pre-range input bars);
//   (b) na-warm its embedded ``ta.ema`` per TradingView's *built-in* semantics
//       — na until ``length`` HTF bars accumulate, then seed with the SMA of
//       those first ``length`` values, then recurse (NOT the engine's default
//       src-seed); and
//   (c) expose plain security expressions (here the completed HTF close) only
//       from the first COMPLETED HTF bar at/after the range start.
//
// This fixture drives one HTF ("60" aggregated from "15") request.security with
// an embedded ta.ema(close, 3) end-to-end through the engine and asserts the
// exact per-completed-bar sequence with the flag ON, then asserts the flag OFF
// run is byte-identical to the prior src-seed behavior (opt-in / no regression).
//
// It FAILS without the fix: without the flag machinery the ON run would drop
// nothing and src-seed the EMA, so both the trimmed-count and the na-warm-seed
// assertions below would fail.

#include <pineforge/engine.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace pineforge;

static int failures = 0;

// Bit-exact: the seed/recursion reference values are chosen to be exact
// doubles, so any drift (or a wrong warmup shape) is a hard mismatch.
static bool exact_eq(double a, double b) {
    if (is_na(a) && is_na(b)) return true;
    if (is_na(a) || is_na(b)) return false;
    return a == b;
}

#define CHECK(cond, tag) do { \
    if (!(cond)) { \
        std::printf("FAIL: %s (line %d)\n", (tag), __LINE__); \
        ++failures; \
    } \
} while (0)

// One HTF request.security ("60" from "15"), lookahead_off, with an embedded
// ta.ema(close, 3). Records every completed HTF bar's plain close and embedded
// EMA value, in order.
class RangeStartWarmupHarness : public BacktestEngine {
public:
    ta::EMA sec_ema_{3};
    std::vector<double> htf_close_seq;
    std::vector<double> htf_ema_seq;

    RangeStartWarmupHarness() {
        register_security_eval(0, "60", "15", /*lookahead_on=*/false,
                               /*gaps_on=*/false);
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0 || !is_complete) {
            return;
        }
        htf_close_seq.push_back(bar.close);
        htf_ema_seq.push_back(sec_ema_.compute(bar.close));
    }

    void on_bar(const Bar&) override {}
};

// 8 hours of 15m bars (4 bars/hour). Hour k's four bars all carry close
// hour_close[k], so the aggregated hourly close is unambiguously hour_close[k].
// Hour 0 (ts 0..2.7e6) lies BEFORE the range start (3.6e6) and is dropped when
// the flag is on. Hour 7 never completes (no following bar), matching the
// aggregator's boundary-flush rule.
static std::vector<Bar> make_feed() {
    const double hour_close[8] = {999.0, 10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0};
    std::vector<Bar> bars;
    for (int h = 0; h < 8; ++h) {
        for (int q = 0; q < 4; ++q) {
            int64_t ts = static_cast<int64_t>(h) * 3600000
                       + static_cast<int64_t>(q) * 900000;
            double c = hour_close[h];
            bars.push_back(Bar{c, c, c, c, 100.0, ts});
        }
    }
    return bars;
}

// range start = start of hour 1 (drops hour 0).
static const double kRangeStartMs = 3600000.0;

void test_flag_on_na_warm_and_trim() {
    RangeStartWarmupHarness strat;
    strat.set_syminfo_metadata("security_range_start_na_warmup", kRangeStartMs);
    auto bars = make_feed();
    strat.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    // (a)+(c): hour 0 trimmed -> completed HTF bars are hours 1..7 (7 of them;
    // hour 7 is flushed as the final bucket at end-of-run). First completed
    // close is hour 1's (10), NOT hour 0's (999).
    const double exp_close[7] = {10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0};
    CHECK(strat.htf_close_seq.size() == 7,
          "flag-on: hour0 trimmed -> 7 completed HTF bars");
    for (std::size_t i = 0; i < strat.htf_close_seq.size() && i < 7; ++i) {
        CHECK(exact_eq(strat.htf_close_seq[i], exp_close[i]),
              "flag-on: completed HTF close sequence");
    }

    // (b): ta.ema(3) na-warm. na, na, then SMA(10,20,30)=20 at the 3rd HTF bar,
    // then EMA recursion with alpha = 2/(3+1) = 0.5:
    //   bar4: 0.5*40 + 0.5*20 = 30
    //   bar5: 0.5*50 + 0.5*30 = 40
    //   bar6: 0.5*60 + 0.5*40 = 50
    //   bar7: 0.5*70 + 0.5*50 = 60
    const double na_ = na<double>();
    const double exp_ema[7] = {na_, na_, 20.0, 30.0, 40.0, 50.0, 60.0};
    CHECK(strat.htf_ema_seq.size() == 7, "flag-on: 7 embedded-EMA values");
    for (std::size_t i = 0; i < strat.htf_ema_seq.size() && i < 7; ++i) {
        CHECK(exact_eq(strat.htf_ema_seq[i], exp_ema[i]),
              "flag-on: embedded ta.ema na-warm + SMA-first-length seed");
    }

    std::printf("test_flag_on_na_warm_and_trim: %s\n",
                failures ? "FAIL" : "ok");
}

void test_flag_off_byte_identical_srcseed() {
    int before = failures;
    RangeStartWarmupHarness strat;  // flag NOT set
    auto bars = make_feed();
    strat.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    // No trim: hour 0 (close 999) is a valid HTF bar -> hours 0..7 complete (8;
    // hour 7 flushed as the final bucket at end-of-run).
    const double exp_close[8] = {999.0, 10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0};
    CHECK(strat.htf_close_seq.size() == 8,
          "flag-off: hour0 NOT trimmed -> 8 completed HTF bars");
    for (std::size_t i = 0; i < strat.htf_close_seq.size() && i < 8; ++i) {
        CHECK(exact_eq(strat.htf_close_seq[i], exp_close[i]),
              "flag-off: completed HTF close sequence");
    }

    // Default src-seed EMA: never na; seeds at the first HTF close (999), then
    //   bar2: 0.5*10  + 0.5*999   = 504.5
    //   bar3: 0.5*20  + 0.5*504.5 = 262.25
    const double exp_ema[4] = {
        999.0, 504.5, 262.25,
        0.5 * 30.0 + 0.5 * 262.25,
    };
    CHECK(strat.htf_ema_seq.size() == 8, "flag-off: 8 embedded-EMA values");
    CHECK(!strat.htf_ema_seq.empty() && !is_na(strat.htf_ema_seq.front()),
          "flag-off: EMA non-na from the first HTF bar (src-seed)");
    for (std::size_t i = 0; i < strat.htf_ema_seq.size() && i < 4; ++i) {
        CHECK(exact_eq(strat.htf_ema_seq[i], exp_ema[i]),
              "flag-off: EMA src-seed recursion (byte-identical to prior)");
    }

    std::printf("test_flag_off_byte_identical_srcseed: %s\n",
                (failures > before) ? "FAIL" : "ok");
}

int main() {
    test_flag_on_na_warm_and_trim();
    test_flag_off_byte_identical_srcseed();
    if (failures) {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("test_security_range_start_na_warmup passed.\n");
    return 0;
}
