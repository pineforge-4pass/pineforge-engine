// Aggregate a 1m OHLCV CSV to <tf> with the engine's own TimeframeAggregator
// (spec §10.1 bar-identity lane, row 1). Output columns match the derived
// feed (corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv).
//
// Construction mirrors the engine's aggregator construction site,
// src/engine_run.cpp:1472-1474:
//     script_tf_agg_ = TimeframeAggregator(effective_script_tf, effective_input_tf,
//                                          syminfo_.timezone, syminfo_.session);
// with the engine's SymInfo defaults (include/pineforge/engine.hpp:1043-1044):
// timezone = "UTC", session = "24x7" -- exactly what this tool passes below
// (effective_input_tf is always "1" here: the tool's whole job is to
// aggregate a 1-minute feed).
//
// Emission gate mirrors the engine's actual per-input-bar loop,
// src/engine_run.cpp:1950-1971 (run_aggregation_bar_loop):
//     AggregatedBar ab = script_tf_agg_.feed(input_bars[i]);
//     const bool completed_on_boundary = ab.is_complete
//         && tf_change(ab.bar.timestamp, input_bars[i].timestamp, script_tf_,
//                      syminfo_.timezone, syminfo_.session);
//     if (!bar_magnifier) {
//         for (auto& state : security_eval_states_) {
//             if (completed_on_boundary && state.publish_gate_tf_seconds > 0) { ... }
//             else { feed_security_eval_state(state, input_bars[i], ab.is_complete); }
//         }
//     }
//     ...
//     if (ab.is_complete) {
//         ... dispatch_bar() (or run_magnified_bar()) ...
//     }
//
// The script bar that reaches the strategy (dispatch_bar / run_magnified_bar,
// i.e. what this tool needs to reproduce for a bar-identity comparison) is
// gated SOLELY by `ab.is_complete` -- that `if` at :1970 is unconditional on
// `completed_on_boundary`. `completed_on_boundary` (`ab.is_complete &&
// tf_change(...)`) instead gates ONLY the request.security publish-at-
// boundary path inside the `security_eval_states_` loop at :1954-1969 (and
// the deferred re-feed at :2043-2055) -- a separate subsystem this
// standalone tool has no equivalent of. Verified directly against the
// TimeframeAggregator RATIO-mode source (src/timeframe.cpp:937-1030): for
// gap-free 1m->15m aggregation the normal count-reached completion
// (`sub_bar_count == ratio`, or the real-end rule) fires on the LAST sub-bar
// of the bucket being completed, which is still inside that same bucket as
// `ab.bar` -- so `tf_change(ab.bar.timestamp, input_bars[i].timestamp, ...)`
// is FALSE on essentially every regular completion. Gating this tool's
// emission on `ab.is_complete && tf_change(...)` (as an earlier reading of
// this brief's ruling proposed) would silently emit almost nothing for the
// ~222k regular buckets and only fire on the boundary-triggered completions
// used for gaps/session edges -- contradicting both the engine's actual
// dispatch condition and this lane's own "~222k bars compared" expectation.
// So: gate on `ab.is_complete` alone, exactly as this tool's step-1
// reference implementation already did.
//
// Trailing partial bucket: the engine's per-input for-loop (src/engine_run.cpp
// :1936-2056) has no post-loop flush of a still-pending partial bucket --
// `agg.has_pending_partial()`/`agg.current()`/`agg.last_completed()` are
// never called from engine_run.cpp. So if the 1m feed's very last bucket
// never reaches completion (by count, real-end, or a later boundary bar),
// the engine simply never dispatches it, and this tool mirrors that by NOT
// flushing anything after the loop either. When that last bucket DOES
// happen to reach completion by the time the feed ends (e.g. the file's
// final row is exactly the bucket's last sub-bar), the ordinary
// `ab.is_complete` path already caught it during the loop -- no separate
// flush needed either way. `trailing_partial` in the JSON records whether
// the source feed's very last bucket-key was left incomplete (diagnostic
// only, not a divergence).
#include <pineforge/bar.hpp>
#include <pineforge/timeframe.hpp>
#include <cstdio>
#include <string>
#include <vector>

using namespace pineforge;

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: aggregate_feed <1m.csv> <tf> <out.csv>\n");
        return 2;
    }
    std::FILE* in = std::fopen(argv[1], "r");
    if (!in) {
        std::fprintf(stderr, "aggregate_feed: cannot open %s\n", argv[1]);
        return 2;
    }
    char line[1024];
    std::vector<Bar> bars;
    // Row receipt (finding 1): every line is either the header, a parsed
    // data row, or a skipped (unparsable) row -- silently dropping rows
    // with no count was the original defect. Line 1 must literally start
    // with "timestamp" to be treated as the header; anything else on line 1
    // falls through to the same parse-or-skip path as every other line, so
    // a header-less feed's first data row is no longer eaten.
    long rows_parsed = 0;
    long rows_skipped = 0;
    long line_no = 0;
    long first_skipped_line = 0;  // 0 = none skipped; else 1-based (finding 5/N5)
    bool first_line = true;
    while (std::fgets(line, sizeof line, in)) {
        ++line_no;
        if (first_line) {
            first_line = false;
            if (std::string(line).rfind("timestamp", 0) == 0) {
                continue;  // header row: not data, not skipped
            }
            // else: no header present -- parse line 1 as data below.
        }
        Bar b{};
        long long ts = 0;
        if (std::sscanf(line, "%lld,%lf,%lf,%lf,%lf,%lf",
                         &ts, &b.open, &b.high, &b.low, &b.close, &b.volume) == 6) {
            b.timestamp = ts;
            bars.push_back(b);
            ++rows_parsed;
        } else {
            ++rows_skipped;
            // N5: record the FIRST skipped line (1-based) so a caller's
            // "rows_skipped=1" receipt doesn't require re-scanning the
            // source file to find which row was bad (task-12-rereview
            // new finding 5).
            if (first_skipped_line == 0) {
                first_skipped_line = line_no;
            }
        }
    }
    std::fclose(in);
    if (bars.empty()) {
        std::fprintf(stderr, "aggregate_feed: no data rows parsed from %s\n", argv[1]);
        return 2;
    }

    // N2: hoisted so the ctor call and the provenance print below can never
    // drift from each other (task-12-rereview new finding 2) --
    // TimeframeAggregator exposes no getters for these, so without a single
    // shared source the print could silently describe a different
    // aggregator than the one actually constructed.
    static constexpr const char* kInputTf = "1";
    static constexpr const char* kTz = "UTC";
    static constexpr const char* kSession = "24x7";

    // Engine construction site (src/engine_run.cpp:1472-1474) + SymInfo
    // defaults (include/pineforge/engine.hpp:1043-1044). input_tf is always
    // "1" -- this tool's whole job is aggregating a 1-minute feed.
    TimeframeAggregator agg(std::string(argv[2]), kInputTf, kTz, kSession);
    // Provenance (finding 5): record what the aggregator was actually
    // constructed with, so the lane can relay it into the JSON instead of
    // asserting it from an unrelated constant.
    std::fprintf(stderr, "aggregator: tf=%s input_tf=%s tz=%s session=%s\n",
                 argv[2], kInputTf, kTz, kSession);

    std::FILE* out = std::fopen(argv[3], "w");
    if (!out) {
        std::fprintf(stderr, "aggregate_feed: cannot open %s for writing\n", argv[3]);
        return 2;
    }
    std::fprintf(out, "timestamp,open,high,low,close,volume\n");

    int64_t last_seen_bucket_start = -1;
    bool last_bucket_completed = false;
    const int64_t bucket_ms = static_cast<int64_t>(internal::tf_to_clock_seconds(argv[2])) * 1000;

    for (const Bar& b : bars) {
        // Engine call site (src/engine_run.cpp:1950): single-argument feed(),
        // no lookahead hint -- the engine's own aggregation loop over a plain
        // in-memory bar array passes none either.
        AggregatedBar ab = agg.feed(b);
        const int64_t this_bucket_start = bucket_ms > 0 ? (b.timestamp - (b.timestamp % bucket_ms)) : b.timestamp;
        if (this_bucket_start != last_seen_bucket_start) {
            last_seen_bucket_start = this_bucket_start;
            last_bucket_completed = false;
        }
        if (ab.is_complete) {
            // Mirrors src/engine_run.cpp:1970's dispatch gate exactly (see
            // the file-header comment above for why this is the sole
            // condition, not `ab.is_complete && tf_change(...)`).
            std::fprintf(out, "%lld,%.17g,%.17g,%.17g,%.17g,%.17g\n",
                         static_cast<long long>(ab.bar.timestamp), ab.bar.open,
                         ab.bar.high, ab.bar.low, ab.bar.close, ab.bar.volume);
            // last_seen_bucket_start == this_bucket_start always holds here
            // (set together just above), so this is the sole condition --
            // dropped the redundant second disjunct (finding 7).
            if (ab.bar.timestamp == last_seen_bucket_start) {
                last_bucket_completed = true;
            } else {
                // Boundary-triggered completion: ab.bar is the PREVIOUS
                // bucket, already accounted for on its own row; the bucket
                // this input bar now opens starts fresh (not yet completed).
                last_bucket_completed = false;
            }
        }
    }
    std::fclose(out);

    // Diagnostic only (see file-header comment): did the source feed's very
    // last bucket-key ever reach completion? For this corpus's real data the
    // answer is "yes" (the file's tail happens to fill its bucket exactly),
    // but the check stays generic for any future feed.
    //
    // Row receipt (finding 1): the lane parses this line and asserts
    // rows_parsed against its own independent 1m row count, so any silent
    // drop upstream of this print is now visible on both sides. N5 appends
    // first_skipped_line (1-based; 0 = none skipped) so a "rows_skipped=1"
    // receipt names the row, not just the count.
    std::fprintf(stderr, "rows_parsed=%ld rows_skipped=%ld trailing_partial=%d first_skipped_line=%ld\n",
                 rows_parsed, rows_skipped, last_bucket_completed ? 0 : 1, first_skipped_line);
    return rows_skipped > 0 ? 1 : 0;
}
