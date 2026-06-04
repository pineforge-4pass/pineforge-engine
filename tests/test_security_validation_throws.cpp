// Tests for the remaining uncovered validation / dispatch arms of
// src/engine_security.cpp that the existing security suites
// (test_security_tf_validation, test_security_lower_tf_script_bound,
// test_security_lower_tf_input_passthrough, test_ltf_buffer_no_leak) do
// NOT exercise:
//
//   1. validate_security_timeframes line ~191: a request.security_lower_tf
//      whose requested TF is NOT finer than input (so it reaches the
//      input-passthrough block) while the script TF parses to <= 0
//      seconds (calendar month "M") -> "script timeframe is unknown".
//
//   2. validate_security_timeframes line ~211: a request.security_lower_tf
//      whose requested TF is >= input, strictly finer than script, an
//      exact divisor of script, but NOT an integer multiple of input
//      -> "is not an integer multiple of input".
//
//   3. security_series_slot_is_new (lines 228-236): all three return
//      arms -- unknown sec_id (default true), lookahead_off (always
//      true), and lookahead_on with current_sub_bar_count > 1 (false)
//      vs <= 1 (true).
//
//   4. feed_security_eval_state aggregator partial branch (lines 380-381):
//      an HTF request.security with lookahead_on=true must emit a partial
//      (is_complete=false) evaluation for every incomplete aggregated
//      sub-bar and increment eval_partial_count.
//
// Style: cassert + plain int main(), mirroring tests/test_security_tf_validation.cpp.
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <pineforge/engine.hpp>

// This suite verifies via assert(). The CLAUDE.md-prescribed gate builds
// Release (-DNDEBUG), which would no-op assert() and make every check vacuous.
// Re-enable assert() unconditionally for this TU, placed after all other
// includes so no earlier/later header can disable it again.
#undef NDEBUG
#include <cassert>

using namespace pineforge;

namespace {

void expect_contains(const std::string& haystack, const std::string& needle,
                     const char* label) {
    if (haystack.find(needle) == std::string::npos) {
        std::cerr << label << ": expected error to contain '" << needle
                  << "' but got: '" << haystack << "'\n";
        assert(false);
    }
}

// ---- Validation-throw harness (mirrors the template's ValidationHarness) ----
// Exposes register_security{,_lower_tf}_eval and never dispatches.
struct ThrowHarness : public BacktestEngine {
    void on_bar(const Bar&) override {}
    void evaluate_security(int, const Bar&, bool) override {}

    void add_security_lower_tf(const std::string& requested_tf) {
        // Empty input_tf at register time so validate_security_timeframes
        // (driven from run() once input_tf_/script_tf_ are known) is the
        // layer under test.
        register_security_lower_tf_eval(0, requested_tf, "");
    }
};

std::vector<Bar> two_bars(int64_t step_ms) {
    return {
        {10.0, 10.0, 10.0, 10.0, 100.0, 0},
        {11.0, 11.0, 11.0, 11.0, 100.0, step_ms},
    };
}

std::string run_throw(ThrowHarness& strat,
                      const std::string& input_tf,
                      const std::string& script_tf,
                      int64_t step_ms) {
    auto bars = two_bars(step_ms);
    strat.run(bars.data(), static_cast<int>(bars.size()),
              input_tf, script_tf, false, 4,
              MagnifierDistribution::ENDPOINTS);
    return strat.last_error();
}

// 1. line ~191: LTF, requested TF == input (not finer, so reaches the
//    input-passthrough block) while script_tf="M" -> tf_to_seconds("M")
//    is -1 (calendar marker), so script_seconds <= 0 fires the
//    "script timeframe is unknown" arm. run()'s ratio check sees
//    tf_ratio("15","M") == -1 (calendar HTF) and does NOT throw early,
//    so validation is reached.
void test_ltf_script_tf_unknown_rejected() {
    ThrowHarness strat;
    strat.add_security_lower_tf("15");          // req == input, not finer
    auto err = run_throw(strat, "15", "M", 900000);
    assert(!err.empty());
    expect_contains(err, "script timeframe is unknown",
                    "test_ltf_script_tf_unknown_rejected");
    expect_contains(err, "request.security_lower_tf",
                    "test_ltf_script_tf_unknown_rejected");
    std::cout << "test_ltf_script_tf_unknown_rejected passed.\n";
}

// 2. line ~211: LTF, input=2m, req=3m, script=6m.
//    req(180s) >= input(120s) -> input-passthrough block.
//    req < script (180 < 360) ok; script % req == 0 (360 % 180 == 0) ok;
//    BUT req % input == 180 % 120 == 60 != 0 -> "is not an integer
//    multiple of input". supports_lower_tf_emulation("2","3") is false
//    (req not finer than input) so the LTF-emulation early-accept is
//    skipped, and run()'s tf_ratio("2","6")==3 so no early throw.
void test_ltf_req_not_integer_multiple_of_input_rejected() {
    ThrowHarness strat;
    strat.add_security_lower_tf("3");
    auto err = run_throw(strat, "2", "6", 120000);
    assert(!err.empty());
    expect_contains(err, "is not an integer multiple of input",
                    "test_ltf_req_not_integer_multiple_of_input_rejected");
    expect_contains(err, "request.security_lower_tf",
                    "test_ltf_req_not_integer_multiple_of_input_rejected");
    std::cout << "test_ltf_req_not_integer_multiple_of_input_rejected passed.\n";
}

// ---- security_series_slot_is_new harness (lines 228-236) ----
// Exposes the protected predicate and lets us hand-craft eval states so
// every return arm is hit deterministically (no run() needed).
struct SlotHarness : public BacktestEngine {
    void on_bar(const Bar&) override {}
    void evaluate_security(int, const Bar&, bool) override {}

    bool slot_is_new(int sec_id) const { return security_series_slot_is_new(sec_id); }

    // Register one HTF security and then tweak its eval-state fields so
    // each branch of security_series_slot_is_new is reachable.
    void add(bool lookahead_on, int sub_bar_count) {
        register_security_eval(7, "60", "15", lookahead_on, false);
        auto& st = security_eval_states_.back();
        st.lookahead_on = lookahead_on;
        st.current_sub_bar_count = sub_bar_count;
    }
};

// 3a. Unknown sec_id -> default true (loop falls through, line 235).
void test_slot_unknown_sec_id_is_new() {
    SlotHarness strat;
    // No securities registered at all.
    assert(strat.slot_is_new(99) == true);
    std::cout << "test_slot_unknown_sec_id_is_new passed.\n";
}

// 3b. lookahead_off -> always new regardless of sub_bar_count (line 233,
//     left operand of ||).
void test_slot_lookahead_off_is_new() {
    SlotHarness strat;
    strat.add(/*lookahead_on=*/false, /*sub_bar_count=*/5);
    assert(strat.slot_is_new(7) == true);
    // A different (unregistered) sec_id still defaults to true.
    assert(strat.slot_is_new(8) == true);
    std::cout << "test_slot_lookahead_off_is_new passed.\n";
}

// 3c. lookahead_on with sub_bar_count > 1 -> NOT new (false);
//     sub_bar_count <= 1 -> new (true). Covers both halves of the
//     "current_sub_bar_count <= 1" comparison (line 233).
void test_slot_lookahead_on_depends_on_sub_bar_count() {
    {
        SlotHarness strat;
        strat.add(/*lookahead_on=*/true, /*sub_bar_count=*/3);
        assert(strat.slot_is_new(7) == false);  // mid-bar: not a new slot
    }
    {
        SlotHarness strat;
        strat.add(/*lookahead_on=*/true, /*sub_bar_count=*/1);
        assert(strat.slot_is_new(7) == true);   // first sub-bar: new slot
    }
    {
        SlotHarness strat;
        strat.add(/*lookahead_on=*/true, /*sub_bar_count=*/0);
        assert(strat.slot_is_new(7) == true);   // count==0 also <= 1: new
    }
    std::cout << "test_slot_lookahead_on_depends_on_sub_bar_count passed.\n";
}

// ---- Partial-eval harness (lines 380-381) ----
// HTF request.security with lookahead_on=true. Each input bar inside an
// aggregation group that does NOT complete the HTF bar must trigger a
// partial (is_complete=false) evaluate_security call.
struct PartialEvalHarness : public BacktestEngine {
    std::vector<std::pair<double, bool>> dispatches;  // (close, is_complete)
    int partial_calls = 0;
    int complete_calls = 0;

    PartialEvalHarness() {
        // input=15m, requested=60m -> ratio 4. lookahead_on=true so the
        // aggregator emits partials for the 3 incomplete sub-bars per group.
        register_security_eval(0, "60", "15", /*lookahead_on=*/true, false);
        security_eval_states_.back().lookahead_on = true;
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        if (sec_id != 0) return;
        dispatches.emplace_back(bar.close, is_complete);
        if (is_complete) complete_calls++;
        else partial_calls++;
    }

    void on_bar(const Bar&) override {}
};

// 4. Eight 15m input bars => two complete 60m HTF bars. With lookahead_on
//    each group emits 3 partials + 1 complete, so 6 partials + 2 completes.
//    The partial close tracks the running aggregate close (== the latest
//    input bar's close at the time, since each input bar's close becomes
//    the partial bar's close). We pin both the counts and the per-step
//    is_complete pattern.
void test_htf_lookahead_emits_partial_evals() {
    PartialEvalHarness strat;
    std::vector<Bar> bars;
    for (int i = 0; i < 8; ++i) {
        double c = 100.0 + i;  // closes 100..107
        bars.push_back({c, c, c, c, 10.0, static_cast<int64_t>(i) * 900000});
    }
    strat.run(bars.data(), static_cast<int>(bars.size()),
              "15", "60", false, 4, MagnifierDistribution::ENDPOINTS);
    assert(strat.last_error().empty());

    // 8 input bars -> 8 dispatches (one per fed input bar).
    assert(strat.dispatches.size() == 8);
    // 2 HTF bars * (3 partials + 1 complete).
    assert(strat.partial_calls == 6);
    assert(strat.complete_calls == 2);

    // is_complete pattern: F F F T  F F F T (complete only on every 4th bar).
    const bool expect_complete[8] = {false, false, false, true,
                                     false, false, false, true};
    for (int i = 0; i < 8; ++i) {
        assert(strat.dispatches[static_cast<std::size_t>(i)].second
               == expect_complete[i]);
    }

    // The bar handed to evaluate_security carries the running aggregate
    // close, which for these monotone bars equals the i-th input close.
    for (int i = 0; i < 8; ++i) {
        double got = strat.dispatches[static_cast<std::size_t>(i)].first;
        double want = 100.0 + i;
        assert(got == want);
    }
    std::cout << "test_htf_lookahead_emits_partial_evals passed.\n";
}

}  // namespace

int main() {
    test_ltf_script_tf_unknown_rejected();
    test_ltf_req_not_integer_multiple_of_input_rejected();
    test_slot_unknown_sec_id_is_new();
    test_slot_lookahead_off_is_new();
    test_slot_lookahead_on_depends_on_sub_bar_count();
    test_htf_lookahead_emits_partial_evals();
    std::cout << "All test_security_validation_throws tests passed.\n";
    return 0;
}
