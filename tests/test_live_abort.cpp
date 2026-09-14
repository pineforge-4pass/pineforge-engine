#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cstdio>
#include <vector>
using namespace pineforge;
namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
Bar flat_bar(double p, int64_t ts) { return Bar{p, p, p, p, 1.0, ts}; }
// Requests its own abort from inside on_bar at bar 5, as a tick thread would.
class AbortAtFive final : public pineforge::source::PineStrategyHost {
public:
    int bars_seen = 0;
    void on_source_bar(const Bar&) override {
        ++bars_seen;
        if (bar_index_ == 5) request_abort();
    }
};
}
int main() {
    std::vector<Bar> bars;
    for (int i = 0; i < 20; ++i) bars.push_back(flat_bar(100.0 + i, i * 60'000LL));

    // --- run(const Bar*, int): the single-timeframe overload ---
    AbortAtFive s;
    s.run(bars.data(), (int)bars.size());
    CHECK(s.last_run_status() == 1);      // NOT_COMPLETED
    CHECK(s.last_error().empty());        // an abort is not an error
    CHECK(s.bars_seen == 6);              // bars 0..5 ran, bar 6 never dispatched
    // The flag is consumed: the SAME (aborted) handle runs to completion.
    s.run(bars.data(), 5);                // aborted handle, run again
    CHECK(s.last_run_status() == 0);
    CHECK(s.last_error().empty());
    CHECK(s.bars_seen == 11);             // 6 from the aborted run + 5

    // A request made while idle is cleared at run() entry: no-op.
    AbortAtFive t;
    t.request_abort();                    // set while idle: cleared at run() entry
    t.run(bars.data(), 5);                // bar 5 never reached -> no new abort
    CHECK(t.last_run_status() == 0);
    CHECK(t.bars_seen == 5);

    // --- Second overload, simple-loop sub-path (input_tf == script_tf, no
    // aggregation) -- exercises this overload's own clear-at-entry and its
    // own AbortRequested catch clause, independent of the first overload. ---
    AbortAtFive u;
    u.run(bars.data(), (int)bars.size(), "1", "1");
    CHECK(u.last_run_status() == 1);
    CHECK(u.last_error().empty());
    CHECK(u.bars_seen == 6);
    u.run(bars.data(), 5, "1", "1");      // reused handle, runs to completion
    CHECK(u.last_run_status() == 0);
    CHECK(u.last_error().empty());
    CHECK(u.bars_seen == 11);

    // --- Second overload, aggregation sub-path (script_tf coarser than
    // input_tf) -- exercises check_abort() in run_aggregation_bar_loop and
    // the cleanup calls in the AbortRequested handler there. 100 1-minute
    // bars aggregate into 20 complete 5-minute script bars. ---
    std::vector<Bar> minute_bars;
    for (int i = 0; i < 100; ++i) minute_bars.push_back(flat_bar(100.0 + i, i * 60'000LL));
    AbortAtFive v;
    v.run(minute_bars.data(), (int)minute_bars.size(), "1", "5");
    CHECK(v.last_run_status() == 1);
    CHECK(v.last_error().empty());
    CHECK(v.bars_seen == 6);              // 6 completed script bars (index 0..5)
    // Reused handle: 5 more 1-minute bars complete exactly one more script
    // bar (index 0) without ever reaching index 5 again, so no re-abort.
    v.run(minute_bars.data(), 5, "1", "5");
    CHECK(v.last_run_status() == 0);
    CHECK(v.last_error().empty());
    CHECK(v.bars_seen == 7);

    return failures == 0 ? 0 : 1;
}
