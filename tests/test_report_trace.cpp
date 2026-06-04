// test_report_trace.cpp — exercises the runtime trace path in engine_report.cpp:
//   - BacktestEngine::intern_trace_name  (name interning + dedup)
//   - BacktestEngine::trace              (trace_enabled_ gate + POD push_back)
//   - BacktestEngine::fill_trace_section (heap copy of trace[] + trace_names[])
//   - BacktestEngine::free_report        (delete[] trace / trace_names)
//
// Pins: trace_len == 2*nbars, trace_names_len == 2 (interning dedups the two
// reused names), per-entry timestamp/bar_index/name_id/value, and the name_id
// -> trace_names[] indexing. Also verifies the gate (no traces when disabled)
// and that free_report nulls + zero-lengths the trace arrays.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <pineforge/engine.hpp>
#include <pineforge/bar.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                     \
        } else {                                                                \
            ++tests_passed;                                                     \
        }                                                                       \
    } while (0)

// Strategy that emits two traces per bar using the SAME two names every bar,
// so interning collapses them to exactly 2 unique names. The values are
// deterministic functions of the bar so we can pin exact expected numbers.
//   "ema_fast" -> bar.close
//   "signal"   -> bar.high - bar.low   (the bar's range)
class TraceStrategy : public BacktestEngine {
public:
    TraceStrategy() {
        initial_capital_ = 100000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
    }

    void on_bar(const Bar& bar) override {
        // Emit via the double overload and via the int overload to exercise the
        // forwarding overload path too. "signal" is computed as an int range.
        trace("ema_fast", bar.close);
        int range = static_cast<int>(bar.high - bar.low);
        trace("signal", range);
    }
};

static void test_trace_disabled_emits_nothing() {
    std::printf("test_trace_disabled_emits_nothing\n");
    TraceStrategy strat;
    // trace_enabled_ defaults to false -> trace() is a no-op.
    CHECK(!strat.trace_enabled());

    Bar bars[] = {
        {100.0, 110.0, 95.0, 102.0, 50, 1000},
        {102.0, 112.0, 99.0, 108.0, 50, 2000},
        {108.0, 118.0, 105.0, 112.0, 50, 3000},
    };
    strat.run(bars, 3);

    ReportC rep;
    std::memset(&rep, 0, sizeof(rep));
    strat.fill_report(&rep);

    // Gate held: no trace records, no name table.
    CHECK(rep.trace_len == 0);
    CHECK(rep.trace == nullptr);
    CHECK(rep.trace_names_len == 0);
    CHECK(rep.trace_names == nullptr);

    BacktestEngine::free_report(&rep);
}

static void test_trace_enabled_records_and_interns() {
    std::printf("test_trace_enabled_records_and_interns\n");

    const int nbars = 5;
    Bar bars[nbars] = {
        {100.0, 110.0, 95.0, 102.0, 50, 1000},
        {102.0, 112.0, 99.0, 108.0, 50, 2000},
        {108.0, 118.0, 105.0, 112.0, 50, 3000},
        {112.0, 120.0, 110.0, 115.0, 50, 4000},
        {115.0, 125.0, 111.0, 119.0, 50, 5000},
    };

    TraceStrategy strat;
    strat.set_trace_enabled(true);
    CHECK(strat.trace_enabled());
    strat.run(bars, nbars);

    ReportC rep;
    std::memset(&rep, 0, sizeof(rep));
    strat.fill_report(&rep);

    // Two traces per bar.
    CHECK(rep.trace_len == 2 * nbars);
    CHECK(rep.trace != nullptr);
    // Two distinct reused names -> interned to exactly 2.
    CHECK(rep.trace_names_len == 2);
    CHECK(rep.trace_names != nullptr);

    // First-occurrence interning order: "ema_fast" (id 0), then "signal" (id 1).
    if (rep.trace_names_len == 2) {
        CHECK(std::strcmp(rep.trace_names[0], "ema_fast") == 0);
        CHECK(std::strcmp(rep.trace_names[1], "signal") == 0);
    }

    // Verify every entry: layout is [ema_fast, signal] per bar in emission
    // order, timestamps/bar_index follow the bar, values match what we emitted.
    if (rep.trace_len == 2 * nbars && rep.trace_names_len == 2) {
        for (int b = 0; b < nbars; ++b) {
            const TraceEntryC& ef = rep.trace[2 * b + 0];
            const TraceEntryC& sg = rep.trace[2 * b + 1];

            // ema_fast entry
            CHECK(ef.name_id == 0);
            CHECK(std::strcmp(rep.trace_names[ef.name_id], "ema_fast") == 0);
            CHECK(ef.bar_index == b);
            CHECK(ef.timestamp == bars[b].timestamp);
            CHECK(std::fabs(ef.value - bars[b].close) < 1e-9);

            // signal entry (int range overload -> double)
            int expected_range = static_cast<int>(bars[b].high - bars[b].low);
            CHECK(sg.name_id == 1);
            CHECK(std::strcmp(rep.trace_names[sg.name_id], "signal") == 0);
            CHECK(sg.bar_index == b);
            CHECK(sg.timestamp == bars[b].timestamp);
            CHECK(std::fabs(sg.value - static_cast<double>(expected_range)) < 1e-9);
        }
    }

    // Pin the very first record's concrete values explicitly (bar 0).
    if (rep.trace_len >= 2) {
        CHECK(rep.trace[0].timestamp == 1000);
        CHECK(rep.trace[0].bar_index == 0);
        CHECK(rep.trace[0].name_id == 0);
        CHECK(std::fabs(rep.trace[0].value - 102.0) < 1e-9);  // bar 0 close

        CHECK(rep.trace[1].timestamp == 1000);
        CHECK(rep.trace[1].bar_index == 0);
        CHECK(rep.trace[1].name_id == 1);
        // bar 0 range = 110 - 95 = 15
        CHECK(std::fabs(rep.trace[1].value - 15.0) < 1e-9);
    }

    // free_report must release and reset the trace arrays.
    BacktestEngine::free_report(&rep);
    CHECK(rep.trace == nullptr);
    CHECK(rep.trace_len == 0);
    CHECK(rep.trace_names == nullptr);
    CHECK(rep.trace_names_len == 0);
}

int main() {
    test_trace_disabled_emits_nothing();
    test_trace_enabled_records_and_interns();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
